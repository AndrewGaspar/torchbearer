// D3D12 capture for the raytracing feasibility study (rt_internal.h, docs/raytracing.md).
//
// Device hooks (MinHook on vkd3d-proton's implementation functions, so they see every call
// whatever vtable patches other features made): resources and heaps (heap type, buffer VA
// ranges), RTV/DSV handles, graphics/compute pipeline states (shader hashes, input layouts, target
// formats) and the shader bytecode. Command-list and queue hooks: a census of N frames, one text
// line per event (state changes, draws, dispatches, barriers counted, PIX events, submissions,
// engine markers from rt.cpp). Everything here is dev-only and armed from the lab verbs; with no
// census running each hook is a call-through plus a thread-local store.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "features/rt/dxc.h"
#include "features/rt/rt_internal.h"
#include "features/rt/shaders/spotlight_dxil.h"

namespace tb::rt {
namespace {

// ---- state -------------------------------------------------------------------------------------

std::mutex g_mu;  // everything below

struct Buf {
  ID3D12Resource* res;
  uint64_t size;
  int heap;  // D3D12_HEAP_TYPE, 0 unknown
  unsigned flags;
};
std::map<uint64_t, Buf> g_bufs;  // GPU VA -> buffer
struct Res {
  D3D12_RESOURCE_DESC desc;
  int heap;
  unsigned init_state;
};
std::unordered_map<ID3D12Resource*, Res> g_res;
std::unordered_map<ID3D12Heap*, int> g_heaps;
std::unordered_map<ID3D12Resource*, unsigned> g_state;  // last barrier StateAfter
struct View {
  ID3D12Resource* res;
  int fmt;
};
std::unordered_map<SIZE_T, View> g_rtv, g_dsv;
struct Pso {
  unsigned id;
  std::string line;
};
std::unordered_map<ID3D12PipelineState*, Pso> g_pso;
unsigned g_pso_next = 1;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_shaders;  // FNV-1a 64 -> bytecode
std::unordered_map<ID3D12PipelineState*, std::string> g_pso_pending;  // created, not yet logged
std::string g_pso_log;  // PSO lines since the last flush
uint64_t g_pso_count = 0, g_pso_graphics = 0, g_pso_compute = 0, g_pso_stream = 0, g_pso_library = 0;
uint64_t g_pso_before_census = 0;

std::atomic<bool> g_dev_hooked{false}, g_lists_hooked{false};
std::atomic<int> g_census{0};  // frames left (0 = off)
std::atomic<bool> g_census_done{false};
std::string g_census_path;
std::string g_log;  // census events
std::unordered_set<ID3D12Resource*> g_described;
unsigned g_frame = 0;

thread_local ID3D12GraphicsCommandList* t_cur = nullptr;

struct Counts {
  unsigned cbv = 0, c32 = 0, tab = 0, srv = 0, uav = 0;
};
std::unordered_map<ID3D12GraphicsCommandList*, Counts> g_counts;

// ---- scene capture state ------------------------------------------------------------------------
// Lock-free pointer -> object tables (open addressing, never shrink): looked up on every draw by
// the engine's recording threads; inserts are rare (new lists, new pipeline states).
template <typename V, size_t N>
struct PtrTable {
  std::atomic<const void*> key[N] = {};
  std::atomic<V*> val[N] = {};
  static size_t h(const void* p) { return size_t((uint64_t(uintptr_t(p)) >> 4) * 0x9e3779b97f4a7c15ull >> 24); }
  V* find(const void* p) const {
    for (size_t i = h(p), n = 0; n < N; i++, n++) {
      const void* k = key[i & (N - 1)].load(std::memory_order_acquire);
      if (k == p) return val[i & (N - 1)].load(std::memory_order_acquire);
      if (!k) return nullptr;
    }
    return nullptr;
  }
  void put(const void* p, V* v) {  // insert or replace
    for (size_t i = h(p), n = 0; n < N; i++, n++) {
      auto& k = key[i & (N - 1)];
      const void* cur = k.load(std::memory_order_acquire);
      if (!cur && k.compare_exchange_strong(cur, p)) cur = p;
      if (cur == p) {
        val[i & (N - 1)].store(v, std::memory_order_release);
        return;
      }
    }
  }
};

// One input-layout element the alpha UV gather may read (TEXCOORD0/1, COLOR0).
struct UvElem {
  uint8_t slot = 0xff, fmt = 0;
  uint16_t off = 0;
};

// A graphics pipeline state the scene capture may record draws of.
struct PsoInfo {
  uint8_t cls = DC_NONE;
  std::atomic<uint8_t> ready{0};  // shader reflection: 0 pending, 1 usable, 2 rejected
  uint8_t pos_slot = 0, pos_fmt = 0;
  uint16_t pos_off = 0;
  uint16_t l2v = 0;
  uint8_t flags = 0;
  int8_t cb_root = -1;  // root parameter of the VS's b0 (space 0)
  uint64_t vs = 0, ps = 0;
  UvElem el_tc0, el_tc1, el_col0;
  ID3D12RootSignature* rs = nullptr;
  // Alpha path (DrawRec), resolved by reflection before ready = 1; uv_mode 0 = opaque for rays.
  uint8_t uv_mode = 0;
  int8_t tex_root = -1;    // root parameter (descriptor table) of the alpha texture
  uint16_t tex_off = 0;    // its descriptor offset from the table's start
  float alpha_thresh = 0.5f;
  uint32_t atlas_off = 0;  // mode 3: byte offset of the atlas table in $Globals
  uint8_t atlas_rows = 0;
};
PtrTable<PsoInfo, 16384> g_psoinfo;

// A shader-resource view the game created (or copied): CPU handle -> texture and view parameters.
// The alpha path resolves a draw's descriptor through it at record time. A plain map (the game's
// bindless heap alone has 264192 slots, more than a fixed open-addressing table should hold).
struct SrvInfo {
  ID3D12Resource* res = nullptr;
  uint16_t fmt = 0, mip0 = 0, mips = 0;
  uint8_t dim = 0;  // D3D12_SRV_DIMENSION
};
std::mutex g_srv_mu;
std::unordered_map<SIZE_T, SrvInfo> g_srvinfo;
bool srv_lookup(SIZE_T h, SrvInfo& out) {
  std::lock_guard lk(g_srv_mu);
  auto it = g_srvinfo.find(h);
  if (it == g_srvinfo.end()) return false;
  out = it->second;
  return true;
}

struct ListSt {
  PsoInfo* pso = nullptr;
  ID3D12PipelineState* raw_pso = nullptr;  // for the spot swap (rt_spot)
  uint64_t cbv[32] = {};
  uint64_t tab[32] = {};  // SetGraphicsRootDescriptorTable handles (GPU ptrs)
  uint64_t heap_cpu0 = 0, heap_gpu0 = 0;  // the bound CBV/SRV/UAV heap's starts
  uint32_t heap_num = 0;
  D3D12_VERTEX_BUFFER_VIEW vb[8] = {};
  D3D12_INDEX_BUFFER_VIEW ib = {};
  int topo = 0;
  uint64_t viewport = 0;
  SIZE_T depth_view = 0;
  ID3D12Resource* rt[2] = {};
  std::vector<DrawRec> draws;
  std::vector<std::pair<ID3D12Resource*, unsigned>> trans;  // transitions, record order
};
PtrTable<ListSt, 4096> g_lists_st;
std::mutex g_lists_mu;  // creating ListSt entries

std::atomic<bool> g_scene{false};
std::atomic<bool> g_full{false};  // dev census bookkeeping (rt_capture) as well as the scene's
std::unordered_map<ID3D12RootSignature*, int> g_rs_cb0;     // root signature -> VS b0 | PS b0 << 8
// Root signature descriptor tables (for the alpha texture's root parameter): per parameter, its
// SRV ranges (base register, space, count, offset from the table's start). Other parameters keep
// an empty range list. Parsed in hk_rs with the versioned deserializer.
struct RsRange {
  uint32_t reg, space, num, off, vis;  // vis: the parameter's D3D12_SHADER_VISIBILITY
};
std::unordered_map<ID3D12RootSignature*, std::vector<std::vector<RsRange>>> g_rs_tabs;
struct HeapEnds {
  uint64_t cpu0, gpu0;
  uint32_t num;
};
std::unordered_map<ID3D12DescriptorHeap*, HeapEnds> g_dheaps;  // shader-visible CBV/SRV/UAV heaps
UINT g_srv_inc = 0;
std::atomic<uint64_t> g_alpha_ok{0}, g_alpha_miss{0};  // alpha draws with / without a texture resolved
std::unordered_map<ID3D12Resource*, void*> g_mapped;        // upload buffers we mapped
std::unordered_map<ID3D12Resource*, unsigned> g_gpu_state;  // state in submission order
std::atomic<uint64_t> g_draws_seen{0}, g_draws_kept{0}, g_draws_inst{0}, g_draws_topo{0}, g_draws_pending{0};
std::atomic<uint64_t> g_refl_ok{0}, g_refl_rej{0}, g_draws_nobind{0}, g_draws_range{0}, g_draws_nonidx{0};

ListSt* list_st(ID3D12GraphicsCommandList* l) {
  ListSt* s = g_lists_st.find(l);
  if (s) return s;
  std::lock_guard lk(g_lists_mu);
  s = g_lists_st.find(l);
  if (!s) {
    s = new ListSt;
    g_lists_st.put(l, s);
  }
  return s;
}

// Serialized root signature (a DXBC container with an RTS0 part): the root parameters that are a
// CBV at b0 space 0 visible to the vertex shader (VERTEX, else ALL) and to the pixel shader (PIXEL,
// else ALL), as vs | ps << 8, 0xff for none.
int rs_vs_cb0(const void* blob, size_t n) {
  auto* p = static_cast<const uint8_t*>(blob);
  auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= n ? *reinterpret_cast<const uint32_t*>(p + o) : 0; };
  size_t rts = SIZE_MAX;
  if (n >= 32 && !memcmp(p, "DXBC", 4)) {
    uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && i < 64; i++) {
      uint32_t off = u32(32 + 4 * i);
      if (off + 8 <= n && !memcmp(p + off, "RTS0", 4)) rts = off + 8;
    }
  }
  if (rts == SIZE_MAX) return 0xffff;
  auto r = [&](size_t o) { return u32(rts + o); };
  uint32_t np = r(4), po = r(8);
  int vs = -1, ps = -1, all = -1;
  for (uint32_t i = 0; i < np && i < 64; i++) {
    uint32_t type = r(po + 12 * i), vis = r(po + 12 * i + 4), pl = r(po + 12 * i + 8);
    if (type != D3D12_ROOT_PARAMETER_TYPE_CBV) continue;
    if (r(pl) != 0 || r(pl + 4) != 0) continue;
    if (vis == D3D12_SHADER_VISIBILITY_VERTEX && vs < 0) vs = int(i);
    if (vis == D3D12_SHADER_VISIBILITY_PIXEL && ps < 0) ps = int(i);
    if (vis == D3D12_SHADER_VISIBILITY_ALL && all < 0) all = int(i);
  }
  if (vs < 0) vs = all;
  if (ps < 0) ps = all;
  return (vs < 0 ? 0xff : vs) | (ps < 0 ? 0xff : ps) << 8;
}

uint64_t fnv(const void* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

void ev(const char* f, ...) __attribute__((format(gnu_printf, 1, 2)));
void ev(const char* f, ...) {  // caller holds g_mu
  char b[1024];
  va_list ap;
  va_start(ap, f);
  int n = vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  if (n < 0) return;
  g_log.append(b, size_t(n) < sizeof b ? size_t(n) : sizeof b - 1);
  g_log += '\n';
}

bool on() { return g_census.load(std::memory_order_relaxed) > 0; }

void describe(ID3D12Resource* r) {  // caller holds g_mu
  if (!r || g_described.count(r)) return;
  g_described.insert(r);
  auto it = g_res.find(r);
  if (it == g_res.end()) {
    D3D12_RESOURCE_DESC d = r->GetDesc();
    ev("R %p %llu %u %u %d dim=%d flags=%#x samples=%u mips=%u heap=? (untracked)", (void*)r,
       (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, int(d.Format), int(d.Dimension), unsigned(d.Flags),
       d.SampleDesc.Count, d.MipLevels);
    return;
  }
  auto& d = it->second.desc;
  ev("R %p %llu %u %u %d dim=%d flags=%#x samples=%u mips=%u heap=%d", (void*)r, (unsigned long long)d.Width, d.Height,
     d.DepthOrArraySize, int(d.Format), int(d.Dimension), unsigned(d.Flags), d.SampleDesc.Count, d.MipLevels,
     it->second.heap);
}

// "ptr+off" of the buffer holding a GPU VA, described once. Caller holds g_mu.
std::string va_str(uint64_t va) {
  if (!va) return "0";
  auto it = g_bufs.upper_bound(va);
  if (it != g_bufs.begin()) {
    --it;
    if (va < it->first + it->second.size) {
      ID3D12Resource* r = it->second.res;
      if (!g_described.count(r)) {
        g_described.insert(r);
        ev("BUF %p %llu heap=%d flags=%#x", (void*)r, (unsigned long long)it->second.size, it->second.heap,
           it->second.flags);
      }
      char b[64];
      snprintf(b, sizeof b, "%p+%llu", (void*)r, (unsigned long long)(va - it->first));
      return b;
    }
  }
  char b[32];
  snprintf(b, sizeof b, "va%llx", (unsigned long long)va);
  return b;
}

void track_resource(ID3D12Resource* r, int heap, unsigned st) {
  if (!r) return;
  D3D12_RESOURCE_DESC d = r->GetDesc();
  std::lock_guard lk(g_mu);
  g_res[r] = Res{d, heap, st};
  g_state[r] = st;
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    uint64_t va = r->GetGPUVirtualAddress();
    if (va) {
      // A new buffer over a range old ones had: drop those (and our mappings of them).
      auto it = g_bufs.lower_bound(va);
      while (it != g_bufs.end() && it->first < va + d.Width) {
        g_mapped.erase(it->second.res);
        it = g_bufs.erase(it);
      }
      g_bufs[va] = Buf{r, d.Width, heap, unsigned(d.Flags)};
      if (g_scene.load(std::memory_order_relaxed)) scene_invalidate(va, d.Width);
    }
  }
}

// ---- pipeline states ---------------------------------------------------------------------------

std::string shader(const D3D12_SHADER_BYTECODE& b) {  // caller holds g_mu
  if (!b.pShaderBytecode || !b.BytecodeLength) return "-";
  uint64_t h = fnv(b.pShaderBytecode, b.BytecodeLength);
  if (!g_shaders.count(h)) {
    auto* p = static_cast<const uint8_t*>(b.pShaderBytecode);
    g_shaders[h].assign(p, p + b.BytecodeLength);
    // Container digest (DXBC header bytes 4..20) and whether it holds DXIL or DXBC code.
    std::string kind = "?";
    char dig[40] = "-";
    if (b.BytecodeLength >= 32 && !memcmp(p, "DXBC", 4)) {
      for (int i = 0; i < 16; i++) snprintf(dig + 2 * i, 3, "%02x", p[4 + i]);
      uint32_t n = *reinterpret_cast<const uint32_t*>(p + 28);
      for (uint32_t i = 0; i < n && 32 + 4 * i + 4 <= b.BytecodeLength; i++) {
        uint32_t off = *reinterpret_cast<const uint32_t*>(p + 32 + 4 * i);
        if (off + 4 > b.BytecodeLength) continue;
        if (!memcmp(p + off, "DXIL", 4)) kind = "DXIL";
        else if ((!memcmp(p + off, "SHEX", 4) || !memcmp(p + off, "SHDR", 4)) && kind == "?") kind = "DXBC";
      }
    }
    char line[160];
    snprintf(line, sizeof line, "SH %016llx %zu %s %s\n", (unsigned long long)h, size_t(b.BytecodeLength), kind.c_str(),
             dig);
    g_pso_log += line;
  }
  char s[24];
  snprintf(s, sizeof s, "%016llx", (unsigned long long)h);
  return s;
}

std::string input_layout(const D3D12_INPUT_LAYOUT_DESC& il) {
  std::string s;
  for (UINT i = 0; i < il.NumElements && il.pInputElementDescs; i++) {
    auto& e = il.pInputElementDescs[i];
    char b[128];
    snprintf(b, sizeof b, "%s%s:%u:%d:%u:%u:%d:%u", i ? ";" : "", e.SemanticName ? e.SemanticName : "?", e.SemanticIndex,
             int(e.Format), e.InputSlot, e.AlignedByteOffset, int(e.InputSlotClass), e.InstanceDataStepRate);
    s += b;
  }
  return s.empty() ? "-" : s;
}

std::string gfx_line(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {  // caller holds g_mu
  std::string rt;
  for (UINT i = 0; i < d.NumRenderTargets && i < 8; i++) rt += (i ? "," : "") + std::to_string(int(d.RTVFormats[i]));
  char b[512];
  snprintf(b, sizeof b,
           "G vs=%s ps=%s ds=%s hs=%s gs=%s rtv=%s dsv=%d samples=%u a2c=%d blend0=%d wmask0=%#x cull=%d dbias=%d "
           "sbias=%g depth=%d dwrite=%d dfunc=%d topo=%d so=%u il=",
           shader(d.VS).c_str(), shader(d.PS).c_str(), shader(d.DS).c_str(), shader(d.HS).c_str(), shader(d.GS).c_str(),
           rt.empty() ? "-" : rt.c_str(), int(d.DSVFormat), d.SampleDesc.Count, d.BlendState.AlphaToCoverageEnable ? 1 : 0,
           d.BlendState.RenderTarget[0].BlendEnable ? 1 : 0, unsigned(d.BlendState.RenderTarget[0].RenderTargetWriteMask),
           int(d.RasterizerState.CullMode), d.RasterizerState.DepthBias, double(d.RasterizerState.SlopeScaledDepthBias),
           d.DepthStencilState.DepthEnable ? 1 : 0, int(d.DepthStencilState.DepthWriteMask),
           int(d.DepthStencilState.DepthFunc), int(d.PrimitiveTopologyType), d.StreamOutput.NumEntries);
  return b + input_layout(d.InputLayout);
}

void add_pso(ID3D12PipelineState* p, const std::string& line) {  // caller holds g_mu
  if (!p) return;
  unsigned id = g_pso_next++;
  g_pso[p] = Pso{id, line};
  char b[48];
  snprintf(b, sizeof b, "PSO %u %p ", id, (void*)p);
  g_pso_log += b + line + "\n";
  g_pso_count++;
  if (on()) ev("PSONEW %u", id);
}

// ---- shader reflection for the scene capture (a background thread) ------------------------------
// From the game's DXC disassembly: the VS's $Globals offsets of g_mLocalToView and g_mViewToWorld
// (which must be 64, as in every captured shader) and whether it has a bone palette (a skinning
// shader: its positions are bind pose); whether the PS discards (alpha test). Cached per hash.
struct ReflJob {
  PsoInfo* info;
  std::vector<uint8_t> vs, ps;
};

std::mutex g_refl_mu;
std::condition_variable g_refl_cv;
std::deque<ReflJob> g_refl_q;
struct VsRefl {
  int l2v = -1;           // g_mLocalToView offset, or -1 rejected
  bool scale_const = false;  // UVs scaled by 1/4095 (R16G16_SINT texcoords; cs_gather mode 1)
  int atlas_row = -1;     // grass: first $Globals float4 row of the atlas table (mode 3)
  int atlas_rows = 0;     // grass: rows per atlas entry
};
struct PsRefl {
  int flags = 0;          // DF_ALPHA, or 0x80 (alpha texture without a test; the A2C rule)
  int tex_reg = -1;       // shader register of the alpha texture (tN, space 0)
  float thresh = 0.5f;    // pass where sampled alpha >= thresh
  bool thresh_ok = false; // a constant threshold was found by the sampled alpha
};
std::unordered_map<uint64_t, VsRefl> g_vs_cache;  // vs hash -> reflection
std::unordered_map<uint64_t, PsRefl> g_ps_cache;  // ps hash -> reflection
bool g_refl_thread = false;

int find_offset(const std::string& t, const char* var) {
  for (size_t p = t.find(var); p != std::string::npos; p = t.find(var, p + 1)) {
    size_t e = t.find('\n', p);
    size_t o = t.find("Offset:", p);
    if (o != std::string::npos && o < e) return atoi(t.c_str() + o + 7);
  }
  return -1;
}

VsRefl reflect_vs(const std::vector<uint8_t>& b) {
  VsRefl r;
  std::string t;
  if (!dxc_disassemble(b.data(), b.size(), t)) return r;
  int l2v = find_offset(t, " g_mLocalToView;");
  int v2w = find_offset(t, " g_mViewToWorld;");
  if (l2v < 0 || v2w != 64) return r;
  for (size_t p = t.find(" x [3 x <4 x float>]]"); p != std::string::npos; p = t.find(" x [3 x <4 x float>]]", p + 1)) {
    size_t s0 = t.rfind('[', p);
    if (s0 != std::string::npos && atoi(t.c_str() + s0 + 1) >= 20) return r;  // bone palette
  }
  r.l2v = l2v;
  // The alpha UV forms (static: the captured shaders, re/scripts uvtree analysis): texcoords are
  // R16G16_SINT scaled by this constant (1/4095), or grass cards index an atlas table in
  // $Globals by int(TEXCOORD0.x * 255 + 0.1), two float4 rows per entry from row 142.
  r.scale_const = t.find("0x3F30010020000000") != std::string::npos;
  size_t m255 = t.find("2.550000e+02");
  if (m255 != std::string::npos) {
    size_t sh = t.find("shl i32", m255);
    size_t c1 = sh == std::string::npos ? sh : t.find(", ", sh);
    int shl = c1 == std::string::npos ? -1 : atoi(t.c_str() + c1 + 2);
    // Several tables are indexed by the same entry; the UV one (offset.xy, scale.zw in one
    // float4) is the load whose components 2 and 3 are also extracted (static: row 142, the
    // row before it feeds only components 0 and 1 elsewhere).
    for (size_t ad = sh == std::string::npos ? std::string::npos : t.find("add i32", sh); ad != std::string::npos;
         ad = t.find("add i32", ad + 1)) {
      size_t ls = t.rfind('\n', ad), eq = t.find(" = ", ls), c2 = t.find(", ", ad);
      if (eq == std::string::npos || eq > ad || c2 == std::string::npos) continue;
      std::string var = t.substr(ls + 1, eq - ls - 1);
      size_t w0 = var.find_first_not_of(" \t");
      if (w0 != std::string::npos) var = var.substr(w0);
      std::string load = "cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %\"$Globals_cbuffer\", i32 " + var + ")";
      size_t lp = t.find(load, ad);
      if (lp == std::string::npos) continue;
      size_t ll = t.rfind('\n', lp), leq = t.find(" = ", ll);
      if (leq == std::string::npos || leq > lp) continue;
      std::string cv = t.substr(ll + 1, leq - ll - 1);
      w0 = cv.find_first_not_of(" \t");
      if (w0 != std::string::npos) cv = cv.substr(w0);
      if (t.find("extractvalue %dx.types.CBufRet.f32 " + cv + ", 3", lp) == std::string::npos) continue;
      int base = atoi(t.c_str() + c2 + 2);
      if (shl >= 0 && shl < 8 && base > 0 && base < 4096) {
        r.atlas_row = base;
        r.atlas_rows = 1 << shl;
      }
      break;
    }
  }
  return r;
}

// A float literal of the disassembly: decimal, or a hex double bit pattern.
bool lit_float(const std::string& s, float& out) {
  if (s.rfind("0x", 0) == 0 && s.size() > 10) {
    uint64_t bits = strtoull(s.c_str(), nullptr, 16);
    double d;
    memcpy(&d, &bits, 8);
    out = float(d);
    return true;
  }
  char* e = nullptr;
  double d = strtod(s.c_str(), &e);
  if (e == s.c_str()) return false;
  out = float(d);
  return true;
}

PsRefl reflect_ps(const std::vector<uint8_t>& b) {
  PsRefl r;
  std::string t;
  if (b.empty() || !dxc_disassemble(b.data(), b.size(), t)) return r;
  // Alpha-tested: the material's alpha-test texture or a foliage card (grass, branches). Neither
  // `discard` (the standard opaque material discards for its dissolve fade) nor alpha-to-coverage
  // (the terrain uses it) alone is one (static: the captured shaders).
  bool tex = t.find("g_sAlphaTestSampler") != std::string::npos || t.find("g_sFoliageNoiseMap") != std::string::npos;
  bool test = t.find("@dx.op.discard") != std::string::npos;
  r.flags = tex && test ? DF_ALPHA : tex ? 0x80 : 0;
  if (!tex) return r;
  // The alpha texture and its register (resource bindings comment), and the threshold: the
  // sampled alpha (extractvalue 3 of its sample) shifted by a constant (the forms are
  // alpha - 0.2 in the shadow depth shaders, derivative-scaled alpha - 0.5 in the G-buffer,
  // alpha - 0.6 for grass; static, the captured shaders). No constant (a $Globals threshold,
  // one shader) leaves thresh_ok false and the geometry opaque for rays.
  const char* name = t.find("g_sAlphaTestSamplerTex") != std::string::npos ? "g_sAlphaTestSamplerTex" : "g_sColorMapTex";
  size_t bp = t.find(std::string("\n; ") + name);
  if (bp != std::string::npos) {
    // The bindings line ends "... T0             t0     1": the register token.
    size_t eol = t.find('\n', bp + 1);
    for (size_t tp = t.find(" t", bp); tp != std::string::npos && tp < eol; tp = t.find(" t", tp + 1))
      if (tp + 2 < eol && isdigit(uint8_t(t[tp + 2])) && (tp < 1 || t[tp - 1] != 'x')) {
        r.tex_reg = atoi(t.c_str() + tp + 2);
        break;
      }
  }
  std::string handle = std::string("%") + name + "_texture_2d, ";
  size_t sp = std::string::npos;
  for (size_t q = t.find(handle); q != std::string::npos; q = t.find(handle, q + 1)) {
    size_t ls2 = t.rfind('\n', q), le2 = t.find('\n', q);
    if (t.find("@dx.op.sample", ls2) < le2) {  // not getDimensions and friends
      sp = q;
      break;
    }
  }
  if (sp == std::string::npos) return r;
  size_t ls = t.rfind('\n', sp);
  size_t eq = t.find(" = ", ls);
  if (eq == std::string::npos || eq > sp) return r;
  std::string res = t.substr(ls + 1, eq - ls - 1);
  size_t ws = res.find_first_not_of(" \t");
  if (ws != std::string::npos) res = res.substr(ws);
  std::string ext = "extractvalue %dx.types.ResRet.f32 " + res + ", 3";
  size_t ep = t.find(ext, sp);
  if (ep == std::string::npos) return r;
  size_t el = t.rfind('\n', ep);
  size_t eeq = t.find(" = ", el);
  if (eeq == std::string::npos || eeq > ep) return r;
  std::string av = t.substr(el + 1, eeq - el - 1);
  ws = av.find_first_not_of(" \t");
  if (ws != std::string::npos) av = av.substr(ws);
  std::vector<std::string> chain{av};
  size_t pos = ep;
  for (int lines = 0; lines < 60 && !r.thresh_ok; lines++) {
    size_t nl = t.find('\n', pos);
    if (nl == std::string::npos) break;
    std::string line = t.substr(pos, nl - pos);
    pos = nl + 1;
    size_t a = line.find(" = f");
    if (a == std::string::npos) continue;
    std::string dst = line.substr(0, a);
    ws = dst.find_first_not_of(" \t");
    if (ws != std::string::npos) dst = dst.substr(ws);
    std::string op = line.substr(a + 3, 4);  // fadd / fsub / fmul
    if (op != "fadd" && op != "fsub" && op != "fmul") continue;
    size_t cm = line.find(", ", a);
    if (cm == std::string::npos) continue;
    std::string x = line.substr(a, cm - a), y = line.substr(cm + 2);
    size_t sp2 = x.rfind(' ');
    x = sp2 == std::string::npos ? x : x.substr(sp2 + 1);
    size_t ye = y.find_first_of(" \t\n");
    if (ye != std::string::npos) y = y.substr(0, ye);
    auto in_chain = [&](const std::string& v) {
      for (auto& c : chain)
        if (c == v) return true;
      return false;
    };
    bool xc = in_chain(x), yc = in_chain(y);
    if (!xc && !yc) continue;
    const std::string& other = xc ? y : x;
    float cst;
    if (lit_float(other, cst)) {
      if (op == "fmul") continue;              // a scale on the alpha itself: not the shift
      r.thresh = op == "fadd" ? -cst : cst;    // alpha + c (c < 0), or alpha - c
      if (r.thresh > -0.25f && r.thresh < 1.25f) r.thresh_ok = true;
    } else if (op == "fmul" && chain.size() < 4) {
      chain.push_back(dst);  // derivative scaling before the shift
    } else {
      break;  // a cbuffer value (or an unrecognised form): opaque for rays
    }
  }
  return r;
}

// The sun / moon shadow resolve (0x52cb40, PS 3fd4eb73c302e5b7 at the captured quality): a pixel
// shader with g_mShadowProjection and g_fRegionStart in its $Globals. Returns the projection's
// offset, or -1.
int reflect_sun_ps(const std::vector<uint8_t>& b) {
  std::string t;
  if (b.empty() || !dxc_disassemble(b.data(), b.size(), t)) return -1;
  if (find_offset(t, " g_fRegionStart;") < 0 || find_offset(t, " g_mViewToWorld;") != 64) return -1;
  return find_offset(t, " g_mShadowProjection;");
}

void refl_loop() {
  for (;;) {
    ReflJob j;
    int l2v, pf;
    {
      std::unique_lock lk(g_refl_mu);
      g_refl_cv.wait(lk, [] { return !g_refl_q.empty(); });
      j = std::move(g_refl_q.front());
      g_refl_q.pop_front();
    }
    if (j.info->cls == DC_SUN) {
      int off = reflect_sun_ps(j.ps);
      if (off >= 0) {
        j.info->l2v = uint16_t(off);
        j.info->ready.store(1, std::memory_order_release);
        g_refl_ok++;
      } else {
        j.info->ready.store(2, std::memory_order_release);
      }
      continue;
    }
    VsRefl vf;
    PsRefl pf2;
    bool have_v = false, have_p = false;
    {
      std::unique_lock lk(g_refl_mu);
      auto a = g_vs_cache.find(j.info->vs);
      auto b = g_ps_cache.find(j.info->ps);
      if (a != g_vs_cache.end()) (vf = a->second), have_v = true;
      if (b != g_ps_cache.end()) (pf2 = b->second), have_p = true;
    }
    if (!have_v) vf = reflect_vs(j.vs);
    if (!have_p) pf2 = reflect_ps(j.ps);
    {
      std::unique_lock lk(g_refl_mu);
      g_vs_cache[j.info->vs] = vf;
      g_ps_cache[j.info->ps] = pf2;
    }
    l2v = vf.l2v;
    pf = pf2.flags;
    if (l2v >= 0) {
      j.info->l2v = uint16_t(l2v);
      j.info->flags |= uint8_t(pf & DF_ALPHA);
      if ((j.info->flags & DF_A2C) && (pf & 0x80)) j.info->flags |= DF_ALPHA;
      // The alpha path (DrawRec): a recognised UV form for this layout, a constant threshold,
      // and the alpha texture's root parameter. Anything missing leaves uv_mode 0 (opaque).
      if ((j.info->flags & DF_ALPHA) && pf2.thresh_ok && pf2.tex_reg >= 0) {
        int mode = 0;
        if (j.info->el_tc0.slot != 0xff && j.info->el_tc0.fmt == 38 && vf.scale_const)
          mode = 1;
        else if (j.info->el_tc0.slot != 0xff && j.info->el_tc0.fmt == 87 && j.info->el_tc1.slot != 0xff &&
                 vf.atlas_row >= 0)
          mode = 3;
        else if (j.info->el_tc0.slot == 0xff && j.info->el_col0.slot != 0xff)
          mode = 2;
        int root = -1;
        uint16_t off = 0;
        if (mode && j.info->rs) {
          // The pixel shader's table (the game's root signature has whole-heap SRV tables for
          // both stages at t0; the vertex one would name a different sliding window).
          std::lock_guard lk2(g_mu);
          auto it = g_rs_tabs.find(j.info->rs);
          if (it != g_rs_tabs.end())
            for (uint32_t want : {uint32_t(D3D12_SHADER_VISIBILITY_PIXEL), uint32_t(D3D12_SHADER_VISIBILITY_ALL)}) {
              for (size_t pi = 0; pi < it->second.size() && root < 0; pi++)
                for (auto& rg : it->second[pi])
                  if (rg.vis == want && rg.space == 0 && uint32_t(pf2.tex_reg) >= rg.reg &&
                      (rg.num == ~0u || uint32_t(pf2.tex_reg) < rg.reg + rg.num)) {
                    root = int(pi);
                    off = uint16_t(rg.off + (uint32_t(pf2.tex_reg) - rg.reg));
                    break;
                  }
              if (root >= 0) break;
            }
        }
        if (mode && root >= 0 && root < 32) {
          j.info->uv_mode = uint8_t(mode);
          j.info->tex_root = int8_t(root);
          j.info->tex_off = off;
          j.info->alpha_thresh = pf2.thresh;
          if (mode == 3) {
            j.info->atlas_off = uint32_t(vf.atlas_row) * 16;
            j.info->atlas_rows = uint8_t(vf.atlas_rows);
          }
          static std::atomic<int> logged{0};
          if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
            TB_LOG("rt capture: alpha path vs %016llx ps %016llx mode %d t%d root %d+%u thresh %.3f",
                   (unsigned long long)j.info->vs, (unsigned long long)j.info->ps, mode, pf2.tex_reg, root, off,
                   double(pf2.thresh));
        }
      }
      j.info->ready.store(1, std::memory_order_release);
      g_refl_ok++;
    } else {
      j.info->ready.store(2, std::memory_order_release);
      g_refl_rej++;
      TB_LOG("rt capture: %s PSO rejected: vs %016llx (l2v %d) ps %016llx", j.info->cls == DC_GBUFFER ? "G-buffer" : "shadow",
             (unsigned long long)j.info->vs, l2v, (unsigned long long)j.info->ps);
    }
  }
}

void refl_enqueue(PsoInfo* info, const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps) {
  ReflJob j;
  j.info = info;
  auto* a = static_cast<const uint8_t*>(vs.pShaderBytecode);
  j.vs.assign(a, a + vs.BytecodeLength);
  if (ps.pShaderBytecode) {
    auto* b = static_cast<const uint8_t*>(ps.pShaderBytecode);
    j.ps.assign(b, b + ps.BytecodeLength);
  }
  std::lock_guard lk(g_refl_mu);
  if (!g_refl_thread) {
    g_refl_thread = true;
    std::thread(refl_loop).detach();
  }
  g_refl_q.push_back(std::move(j));
  g_refl_cv.notify_one();
}

// Classify a graphics PSO by its targets (docs/raytracing.md section 2): the G-buffer writes
// R32_FLOAT + RGBA16F + BGRA8 with D32S8, the shadow atlas R32_FLOAT with D24S8.
void scene_pso(ID3D12PipelineState* p, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, uint64_t vsh, uint64_t psh) {
  uint8_t cls = DC_NONE;
  if (d.NumRenderTargets == 3 && d.RTVFormats[0] == DXGI_FORMAT_R32_FLOAT &&
      d.RTVFormats[1] == DXGI_FORMAT_R16G16B16A16_FLOAT && d.RTVFormats[2] == DXGI_FORMAT_B8G8R8A8_UNORM &&
      d.DSVFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    cls = DC_GBUFFER;
  else if (d.NumRenderTargets == 1 && d.RTVFormats[0] == DXGI_FORMAT_R32_FLOAT &&
           d.DSVFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
    cls = DC_SHADOW;
  else if (d.NumRenderTargets == 1 && d.RTVFormats[0] == DXGI_FORMAT_B8G8R8A8_UNORM && d.PS.pShaderBytecode &&
           (d.BlendState.RenderTarget[0].RenderTargetWriteMask & 7) == 7)
    cls = DC_SUN;  // a candidate: the reflection decides
  if (!cls || d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE || !d.VS.pShaderBytecode) {
    if (g_psoinfo.find(p)) g_psoinfo.put(p, nullptr);  // a recycled address
    return;
  }
  auto* info = new PsoInfo;
  info->cls = cls;
  info->vs = vsh;
  info->ps = psh;
  info->rs = d.pRootSignature;
  if (d.BlendState.AlphaToCoverageEnable) info->flags |= DF_A2C;
  bool pos = false;
  for (UINT i = 0; i < d.InputLayout.NumElements && d.InputLayout.pInputElementDescs; i++) {
    auto& e = d.InputLayout.pInputElementDescs[i];
    if (e.SemanticName && !_stricmp(e.SemanticName, "POSITION") && e.SemanticIndex == 0 &&
        e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && e.InputSlot < 8 &&
        e.AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT &&
        (e.Format == DXGI_FORMAT_R32G32B32_FLOAT || e.Format == DXGI_FORMAT_R32G32B32A32_FLOAT)) {
      info->pos_slot = uint8_t(e.InputSlot);
      info->pos_off = uint16_t(e.AlignedByteOffset);
      info->pos_fmt = uint8_t(e.Format);
      pos = true;
    }
    // Pre-skinned vertices (the Skinning pass's output, rewritten every update) keep the bone
    // indices in their layout (docs/raytracing.md section 5).
    if (e.SemanticName && !_stricmp(e.SemanticName, "BLENDINDICES")) info->flags |= DF_SKINNED;
    // The elements the alpha UV gather reads (per-vertex, explicit offsets).
    if (e.SemanticName && e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && e.InputSlot < 8 &&
        e.AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT) {
      UvElem el{uint8_t(e.InputSlot), uint8_t(e.Format), uint16_t(e.AlignedByteOffset)};
      if (!_stricmp(e.SemanticName, "TEXCOORD") && e.SemanticIndex == 0) info->el_tc0 = el;
      if (!_stricmp(e.SemanticName, "TEXCOORD") && e.SemanticIndex == 1) info->el_tc1 = el;
      if (!_stricmp(e.SemanticName, "COLOR") && e.SemanticIndex == 0) info->el_col0 = el;
    }
  }
  {
    std::lock_guard lk(g_mu);
    auto it = g_rs_cb0.find(d.pRootSignature);
    int v = it != g_rs_cb0.end() ? (cls == DC_SUN ? it->second >> 8 : it->second) & 0xff : 0xff;
    info->cb_root = v == 0xff ? -1 : int8_t(v);
  }
  if (cls == DC_SUN) pos = true;  // full-screen draws: no geometry
  if (!pos || info->cb_root < 0) info->ready = 2;
  g_psoinfo.put(p, info);
  if (!info->ready) refl_enqueue(info, d.VS, d.PS);
}

using CreateGfx_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateCs_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreatePipelineLibrary_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, SIZE_T, REFIID, void**);
using CreateCommitted_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateHeap_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using CreatePlaced_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateView_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);

CreateGfx_t o_gfx;
CreateCs_t o_cs;
CreateStream_t o_stream;
CreatePipelineLibrary_t o_lib;
CreateCommitted_t o_committed;
CreateHeap_t o_heap;
CreatePlaced_t o_placed;
CreateView_t o_rtv, o_dsv;

// ---- shadowed spot light (flashlight) shader swap, [dev] rt_spot (prototype) -------------------
// 1: the game's spotlightPS (aa17e58e1bd8d853, DeferredLight) replaced at pipeline creation by its
//    HLSL port (shaders/spotlight.hlsl, the same PCF: verifies the port and the swap);
// 2: the port with inline ray queries towards the light instead of the shadow atlas. Its TLAS and
//    constants need two root parameters the game's root signatures don't have: every root
//    signature the game creates gets them appended (t0 and b0 in space 9, pixel shader), and they
//    are set right before each draw with a swapped pipeline (scene_spot_bind, scene.cpp).
constexpr uint64_t kSpotPs = 0xaa17e58e1bd8d853ull;
std::atomic<int> g_spot{0};
std::atomic<bool> g_spot_on{true};
std::unordered_map<ID3D12RootSignature*, int> g_rs_extra;          // root signature -> first extra param
std::unordered_map<ID3D12PipelineState*, ID3D12PipelineState*> g_spot_pso;  // swapped -> original
std::unordered_map<ID3D12PipelineState*, int> g_spot_base;        // swapped -> extra param index
std::atomic<uint64_t> g_spot_swaps{0}, g_spot_draws{0}, g_spot_fallback{0};

HRESULT STDMETHODCALLTYPE hk_gfx(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
  HRESULT hr = o_gfx(d, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc && g_spot.load() && desc->PS.pShaderBytecode &&
      fnv(desc->PS.pShaderBytecode, desc->PS.BytecodeLength) == kSpotPs) {
    int mode = g_spot.load(), base = -1;
    {
      std::lock_guard lk(g_mu);
      auto it = g_rs_extra.find(desc->pRootSignature);
      if (it != g_rs_extra.end()) base = it->second;
    }
    if (mode != 2 || base >= 0) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d2 = *desc;
      d2.PS.pShaderBytecode = mode == 2 ? kSpotRqDxil : mode == 3 ? kSpotNoneDxil : kSpotPcfDxil;
      d2.PS.BytecodeLength = mode == 2 ? sizeof kSpotRqDxil : mode == 3 ? sizeof kSpotNoneDxil : sizeof kSpotPcfDxil;
      static std::vector<uint8_t> rt_ps;  // mode 4: the PCF port compiled by the game's DXC 1.4
      if (mode == 4) {
        std::string err;
        std::string src = std::string("#define TB_SHADOW_PCF 1\n") + kSpotHlsl;
        if (rt_ps.empty() && !dxc_compile(src.c_str(), L"main", L"ps_6_0", rt_ps, err))
          TB_LOG("rt spot: in-process compile failed: %s", err.c_str());
        d2.PS.pShaderBytecode = rt_ps.data();
        d2.PS.BytecodeLength = rt_ps.size();
      }
      ID3D12PipelineState* sw = nullptr;
      HRESULT h2 = o_gfx(d, &d2, iid, (void**)&sw);
      if (SUCCEEDED(h2) && sw) {
        std::lock_guard lk(g_mu);
        g_spot_pso[sw] = static_cast<ID3D12PipelineState*>(*out);  // keeps the original alive (fallback)
        g_spot_base[sw] = base;
        *out = sw;
        g_spot_swaps++;
        TB_LOG("rt spot: spotlightPS swapped (%s), pipeline %p (original %p)", mode == 2 ? "ray query" : "PCF port",
               (void*)sw, (void*)g_spot_pso[sw]);
      } else {
        TB_LOG("rt spot: the swapped pipeline failed (hr %#lx)", (unsigned long)h2);
      }
    }
  }
  if (SUCCEEDED(hr) && out && *out && desc) {
    auto hash = [](const D3D12_SHADER_BYTECODE& b) {
      return b.pShaderBytecode ? fnv(b.pShaderBytecode, b.BytecodeLength) : 0ull;
    };
    scene_pso(static_cast<ID3D12PipelineState*>(*out), *desc, hash(desc->VS), hash(desc->PS));
    if (g_full) {
      std::lock_guard lk(g_mu);
      g_pso_graphics++;
      add_pso(static_cast<ID3D12PipelineState*>(*out), gfx_line(*desc));
    }
  }
  return hr;
}

using CreateRS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
CreateRS_t o_rs;

using SerializeVRS_t = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using CreateVDeser_t = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

// The game's root signature with the spotlight's parameters appended (spotlight.hlsl: SRV t0
// space9 TLAS, CBV b0 space9, SRV t1 space9 alpha buffer, a descriptor table over the game's
// own heap at t0 space10, static sampler s0 space9), or an empty vector. Appended at the end,
// so every original index keeps its meaning; only installed with the RT flashlight on.
std::vector<uint8_t> rs_extend(const void* blob, size_t n, int* base) {
  HMODULE m = GetModuleHandleW(L"d3d12.dll");
  auto deser = m ? reinterpret_cast<CreateVDeser_t>(GetProcAddress(m, "D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
  auto ser = m ? reinterpret_cast<SerializeVRS_t>(GetProcAddress(m, "D3D12SerializeVersionedRootSignature")) : nullptr;
  std::vector<uint8_t> out;
  if (!deser || !ser) return out;
  ID3D12VersionedRootSignatureDeserializer* ds = nullptr;
  if (FAILED(deser(blob, n, __uuidof(ID3D12VersionedRootSignatureDeserializer), (void**)&ds)) || !ds) return out;
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
  if (SUCCEEDED(ds->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = *vd;
    std::vector<D3D12_ROOT_PARAMETER1> ps(d.Desc_1_1.pParameters, d.Desc_1_1.pParameters + d.Desc_1_1.NumParameters);
    *base = int(ps.size());
    D3D12_ROOT_PARAMETER1 a{}, b{}, c{}, t4{};
    a.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    a.Descriptor.ShaderRegister = 0;
    a.Descriptor.RegisterSpace = 9;
    a.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    a.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    b = a;
    b.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    c = a;
    c.Descriptor.ShaderRegister = 1;
    static const D3D12_DESCRIPTOR_RANGE1 kHeapRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ~0u, 0, 10,
                                                    D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    t4.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    t4.DescriptorTable.NumDescriptorRanges = 1;
    t4.DescriptorTable.pDescriptorRanges = &kHeapRange;
    t4.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ps.push_back(a);
    ps.push_back(b);
    ps.push_back(c);
    ps.push_back(t4);
    d.Desc_1_1.NumParameters = UINT(ps.size());
    d.Desc_1_1.pParameters = ps.data();
    std::vector<D3D12_STATIC_SAMPLER_DESC> ss(d.Desc_1_1.pStaticSamplers,
                                              d.Desc_1_1.pStaticSamplers + d.Desc_1_1.NumStaticSamplers);
    D3D12_STATIC_SAMPLER_DESC wrap{};
    wrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    wrap.AddressU = wrap.AddressV = wrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    wrap.MaxLOD = D3D12_FLOAT32_MAX;
    wrap.ShaderRegister = 0;
    wrap.RegisterSpace = 9;
    wrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ss.push_back(wrap);
    d.Desc_1_1.NumStaticSamplers = UINT(ss.size());
    d.Desc_1_1.pStaticSamplers = ss.data();
    ID3DBlob *o = nullptr, *e = nullptr;
    if (SUCCEEDED(ser(&d, &o, &e)) && o) {
      auto* p = static_cast<const uint8_t*>(o->GetBufferPointer());
      out.assign(p, p + o->GetBufferSize());
    }
    if (o) o->Release();
    if (e) e->Release();
  }
  ds->Release();
  return out;
}

// The root signature's descriptor-table SRV ranges, per root parameter (g_rs_tabs).
std::vector<std::vector<RsRange>> rs_tabs(const void* blob, size_t n) {
  std::vector<std::vector<RsRange>> out;
  HMODULE m = GetModuleHandleW(L"d3d12.dll");
  auto deser = m ? reinterpret_cast<CreateVDeser_t>(GetProcAddress(m, "D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
  if (!deser) return out;
  ID3D12VersionedRootSignatureDeserializer* ds = nullptr;
  if (FAILED(deser(blob, n, __uuidof(ID3D12VersionedRootSignatureDeserializer), (void**)&ds)) || !ds) return out;
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
  if (SUCCEEDED(ds->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
    out.resize(vd->Desc_1_1.NumParameters);
    for (UINT i = 0; i < vd->Desc_1_1.NumParameters; i++) {
      const auto& p = vd->Desc_1_1.pParameters[i];
      if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
      uint32_t append = 0;
      for (UINT k = 0; k < p.DescriptorTable.NumDescriptorRanges; k++) {
        const auto& rg = p.DescriptorTable.pDescriptorRanges[k];
        uint32_t off = rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                           ? append
                           : rg.OffsetInDescriptorsFromTableStart;
        append = off + (rg.NumDescriptors == ~0u ? 0 : rg.NumDescriptors);
        if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
          out[i].push_back({rg.BaseShaderRegister, rg.RegisterSpace, rg.NumDescriptors, off,
                            uint32_t(p.ShaderVisibility)});
      }
    }
  }
  ds->Release();
  return out;
}

HRESULT STDMETHODCALLTYPE hk_rs(ID3D12Device* d, UINT mask, const void* blob, SIZE_T n, REFIID iid, void** out) {
  if (g_spot.load() == 2 && blob) {
    int base = -1;
    std::vector<uint8_t> ext = rs_extend(blob, n, &base);
    if (!ext.empty()) {
      HRESULT hr = o_rs(d, mask, ext.data(), ext.size(), iid, out);
      if (SUCCEEDED(hr) && out && *out) {
        int i = rs_vs_cb0(blob, n);
        auto tabs = rs_tabs(blob, n);
        std::lock_guard lk(g_mu);
        g_rs_cb0[static_cast<ID3D12RootSignature*>(*out)] = i;
        g_rs_tabs[static_cast<ID3D12RootSignature*>(*out)] = std::move(tabs);
        g_rs_extra[static_cast<ID3D12RootSignature*>(*out)] = base;
        return hr;
      }
      TB_LOG("rt spot: the extended root signature failed (hr %#lx); the game's own instead", (unsigned long)hr);
    }
  }
  HRESULT hr = o_rs(d, mask, blob, n, iid, out);
  if (SUCCEEDED(hr) && out && *out && blob) {
    int i = rs_vs_cb0(blob, n);
    auto tabs = rs_tabs(blob, n);
    static std::atomic<int> rslogged{0};
    if (rslogged.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::string s;
      for (size_t pi = 0; pi < tabs.size(); pi++)
        for (auto& rg : tabs[pi]) {
          char b[96];
          snprintf(b, sizeof b, " [p%zu t%u s%u n%u +%u]", pi, rg.reg, rg.space, rg.num, rg.off);
          s += b;
        }
      TB_LOG("rt capture: root sig %p params %zu srv ranges:%s", *out, tabs.size(), s.empty() ? " none" : s.c_str());
    }
    std::lock_guard lk(g_mu);
    g_rs_cb0[static_cast<ID3D12RootSignature*>(*out)] = i;
    g_rs_tabs[static_cast<ID3D12RootSignature*>(*out)] = std::move(tabs);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_cs(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
  HRESULT hr = o_cs(d, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc && g_full) {
    std::lock_guard lk(g_mu);
    g_pso_compute++;
    add_pso(static_cast<ID3D12PipelineState*>(*out), "C cs=" + shader(desc->CS));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_stream(ID3D12Device* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out) {
  HRESULT hr = o_stream(d, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc && g_full) {
    // Only the shader stages are pulled out of the stream (each subobject: an aligned type tag,
    // then its data; shader subobjects are a D3D12_SHADER_BYTECODE).
    std::lock_guard lk(g_mu);
    g_pso_stream++;
    std::string line = "S";
    auto* p = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    size_t off = 0;
    while (p && off + sizeof(void*) <= desc->SizeInBytes) {
      int type = *reinterpret_cast<const int*>(p + off);
      size_t data = off + sizeof(void*);
      size_t sz = 0;
      switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: {
          static const char* nm[] = {"", "vs", "ps", "ds", "hs", "gs", "cs"};
          line += std::string(" ") + nm[type] + "=" + shader(*reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + data));
          sz = sizeof(D3D12_SHADER_BYTECODE);
          break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: sz = sizeof(void*); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
          line += " il=" + input_layout(*reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC*>(p + data));
          sz = sizeof(D3D12_INPUT_LAYOUT_DESC);
          break;
        default: sz = 0; break;
      }
      if (!sz) break;  // a subobject this parser does not size: stop
      off = (data + sz + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    add_pso(static_cast<ID3D12PipelineState*>(*out), line);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_lib(ID3D12Device* d, const void* blob, SIZE_T n, REFIID iid, void** out) {
  HRESULT hr = o_lib(d, blob, n, iid, out);
  std::lock_guard lk(g_mu);
  g_pso_library++;
  g_pso_log += "LIB " + std::to_string(n) + " hr=" + std::to_string(long(hr)) + "\n";
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_committed(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                       const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
                                       REFIID iid, void** out) {
  HRESULT hr = o_committed(d, hp, hf, desc, st, cv, iid, out);
  if (SUCCEEDED(hr) && out && *out && iid == __uuidof(ID3D12Resource))
    track_resource(static_cast<ID3D12Resource*>(*out), hp ? int(hp->Type) : 0, unsigned(st));
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_heap(ID3D12Device* d, const D3D12_HEAP_DESC* desc, REFIID iid, void** out) {
  HRESULT hr = o_heap(d, desc, iid, out);
  if (SUCCEEDED(hr) && out && *out && desc) {
    std::lock_guard lk(g_mu);
    g_heaps[static_cast<ID3D12Heap*>(*out)] = int(desc->Properties.Type);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_placed(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* desc,
                                    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID iid, void** out) {
  HRESULT hr = o_placed(d, h, off, desc, st, cv, iid, out);
  if (SUCCEEDED(hr) && out && *out && iid == __uuidof(ID3D12Resource)) {
    int ht = 0;
    {
      std::lock_guard lk(g_mu);
      auto it = g_heaps.find(h);
      if (it != g_heaps.end()) ht = it->second;
    }
    track_resource(static_cast<ID3D12Resource*>(*out), ht, unsigned(st));
  }
  return hr;
}

void STDMETHODCALLTYPE hk_rtv(ID3D12Device* d, ID3D12Resource* r, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
  o_rtv(d, r, desc, h);
  std::lock_guard lk(g_mu);
  int f = desc ? int(static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(desc)->Format) : -1;
  g_rtv[h.ptr] = View{r, f};
}

void STDMETHODCALLTYPE hk_dsv(ID3D12Device* d, ID3D12Resource* r, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
  o_dsv(d, r, desc, h);
  std::lock_guard lk(g_mu);
  int f = desc ? int(static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc)->Format) : -1;
  g_dsv[h.ptr] = View{r, f};
}

CreateView_t o_srvv;
using CopyDesc_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescSimple_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
CopyDesc_t o_copyDesc;
CopyDescSimple_t o_copyDescSimple;

void STDMETHODCALLTYPE hk_srvv(ID3D12Device* d, ID3D12Resource* r, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
  o_srvv(d, r, desc, h);
  SrvInfo info;
  info.res = r;
  const auto* vd = static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
  if (vd) {
    info.fmt = uint16_t(vd->Format);
    info.dim = uint8_t(vd->ViewDimension);
    if (vd->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
      info.mip0 = uint16_t(vd->Texture2D.MostDetailedMip);
      info.mips = uint16_t(vd->Texture2D.MipLevels);
    }
  } else if (r) {
    D3D12_RESOURCE_DESC rd = r->GetDesc();
    info.fmt = uint16_t(rd.Format);
    info.dim = uint8_t(D3D12_SRV_DIMENSION_TEXTURE2D);
    info.mips = rd.MipLevels;
  }
  std::lock_guard lk(g_srv_mu);
  g_srvinfo[h.ptr] = info;
}

void STDMETHODCALLTYPE hk_copyDescSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                         D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  o_copyDescSimple(d, n, dst, src, type);
  if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_srv_inc) return;
  std::lock_guard lk(g_srv_mu);
  for (UINT i = 0; i < n; i++) {
    auto it = g_srvinfo.find(src.ptr + SIZE_T(i) * g_srv_inc);
    if (it != g_srvinfo.end()) g_srvinfo[dst.ptr + SIZE_T(i) * g_srv_inc] = it->second;
  }
}

void STDMETHODCALLTYPE hk_copyDesc(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dsts, const UINT* dsz,
                                   UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* srcs, const UINT* ssz,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type) {
  o_copyDesc(d, nd, dsts, dsz, ns, srcs, ssz, type);
  if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_srv_inc || !dsts || !srcs) return;
  // Flattened in order (a null size array means one descriptor per range).
  std::vector<SIZE_T> from, to;
  for (UINT i = 0; i < ns; i++)
    for (UINT k = 0, cnt = ssz ? ssz[i] : 1; k < cnt; k++) from.push_back(srcs[i].ptr + SIZE_T(k) * g_srv_inc);
  for (UINT i = 0; i < nd; i++)
    for (UINT k = 0, cnt = dsz ? dsz[i] : 1; k < cnt; k++) to.push_back(dsts[i].ptr + SIZE_T(k) * g_srv_inc);
  std::lock_guard lk(g_srv_mu);
  for (size_t i = 0; i < from.size() && i < to.size(); i++) {
    auto it = g_srvinfo.find(from[i]);
    if (it != g_srvinfo.end()) g_srvinfo[to[i]] = it->second;
  }
}

using CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDevice_t o_createDevice;
HRESULT WINAPI hk_createDevice(IUnknown* a, D3D_FEATURE_LEVEL fl, REFIID iid, void** out) {
  HRESULT hr = o_createDevice(a, fl, iid, out);
  if (SUCCEEDED(hr) && out && *out) {
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(ID3D12Device), (void**)&dev)) && dev) {
      capture_device(dev);
      dev->Release();
    }
  }
  return hr;
}

void* slot(void* obj, size_t i) { return (*reinterpret_cast<void***>(obj))[i]; }

bool hook_slot(void* obj, size_t i, void* det, void** orig, const char* name) {
  return hook_fn(slot(obj, i), det, orig, name);
}

// ---- command lists -----------------------------------------------------------------------------

using Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawI_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawII_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using Topo_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using Viewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using SetPso_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using Barrier_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using RootAddr_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using RootTable_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using Root32s_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using Root32_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using IB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using VB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using OM_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearDS_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT,
                                           UINT8, UINT, const D3D12_RECT*);
using ClearRT_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT,
                                           const D3D12_RECT*);
using Event_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const void*, UINT);
using EndEvent_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ExecIndirect_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*,
                                                UINT64, ID3D12Resource*, UINT64);
using CopyRes_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTex_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                           const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyBuf_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64,
                                           UINT64);
using Exec_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using SetHeaps_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

Close_t o_close;
Reset_t o_reset;
DrawI_t o_drawI;
DrawII_t o_drawII;
Dispatch_t o_dispatch;
Topo_t o_topo;
Viewports_t o_viewports;
SetPso_t o_setPso;
Barrier_t o_barrier;
RootAddr_t o_gCbv, o_gSrv, o_gUav, o_cCbv, o_cSrv, o_cUav;
RootTable_t o_gTab, o_cTab;
Root32s_t o_g32s, o_c32s;
Root32_t o_g32, o_c32;
IB_t o_ib;
VB_t o_vb;
OM_t o_om;
ClearDS_t o_clearDS;
ClearRT_t o_clearRT;
Event_t o_beginEvent, o_setMarker;
EndEvent_t o_endEvent;
ExecIndirect_t o_execIndirect;
SetHeaps_t o_setHeaps;
CopyRes_t o_copyRes;
CopyTex_t o_copyTex;
CopyBuf_t o_copyBuf;
Exec_t o_exec;

unsigned pso_id(ID3D12PipelineState* p) {  // caller holds g_mu
  if (!p) return 0;
  auto it = g_pso.find(p);
  return it == g_pso.end() ? 0xffffffffu : it->second.id;
}

void counts_line(ID3D12GraphicsCommandList* l, char* b, size_t n) {  // caller holds g_mu
  auto& c = g_counts[l];
  snprintf(b, n, "cbv=%u c32=%u tab=%u srv=%u", c.cbv, c.c32, c.tab, c.srv);
  c = Counts{};
}

// A draw on a G-buffer / shadow pipeline whose shaders are reflected: record its geometry.
void scene_draw(ID3D12GraphicsCommandList* l, UINT ic, UINT n, UINT si, INT bv) {
  ListSt* s = list_st(l);
  PsoInfo* p = s->pso;
  if (!p || p->cls == DC_SUN) return;
  g_draws_seen.fetch_add(1, std::memory_order_relaxed);
  uint8_t ready = p->ready.load(std::memory_order_acquire);
  if (ready != 1) {
    if (!ready) g_draws_pending.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (n != 1) {
    g_draws_inst.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (s->topo != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
    g_draws_topo.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto& vb = s->vb[p->pos_slot];
  const auto& ib = s->ib;
  uint64_t cb = s->cbv[p->cb_root & 31];
  if (!vb.BufferLocation || !vb.StrideInBytes || !ib.BufferLocation || !cb || ic < 3) {
    g_draws_nobind.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  unsigned isz = ib.Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
  if (uint64_t(si + ic) * isz > ib.SizeInBytes || bv < 0) {
    g_draws_range.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint64_t first = uint64_t(bv) * vb.StrideInBytes + p->pos_off;
  if (first + 12 > vb.SizeInBytes) {
    g_draws_range.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  DrawRec d;
  d.cls = p->cls;
  d.flags = p->flags;
  d.ifmt = uint8_t(ib.Format);
  d.vfmt = p->pos_fmt;
  d.stride = vb.StrideInBytes;
  d.vb_view = vb.BufferLocation;
  d.vb = vb.BufferLocation + first;
  d.vcount = uint32_t((vb.SizeInBytes - first - 12) / vb.StrideInBytes + 1);
  d.ib = ib.BufferLocation + uint64_t(si) * isz;
  d.icount = ic;
  d.l2v = p->l2v;
  d.cb = cb;
  d.ps = p->ps;
  if (p->cls == DC_SHADOW && s->viewport && s->depth_view)
    d.shadow_tile = (s->viewport ^ s->depth_view) * 1099511628211ull;
  // The alpha path (DrawRec): the UV stream this draw's alpha sample reads, and its texture's
  // descriptor in the heap bound on this list. Anything unresolved leaves uv_mode / alpha_idx
  // unset and the instance opaque for rays.
  if ((p->flags & DF_ALPHA) && p->uv_mode) {
    const UvElem& ea = p->uv_mode == 2 ? p->el_col0 : p->el_tc0;
    const auto& uvb = s->vb[ea.slot & 7];
    bool slots_ok = p->uv_mode != 3 || (p->el_tc1.slot == p->el_tc0.slot);
    if (slots_ok && uvb.BufferLocation && uvb.StrideInBytes &&
        uint64_t(bv) * uvb.StrideInBytes + ea.off + 4 <= uvb.SizeInBytes) {
      d.uv_mode = p->uv_mode;
      d.uv_fmt_a = ea.fmt;
      d.uv_off_a = ea.off;
      d.uv_vb = uvb.BufferLocation + uint64_t(bv) * uvb.StrideInBytes;
      d.uv_stride = uvb.StrideInBytes;
      d.uv_scale = 0.00024420026f;  // mode 1: the VS's constant (1/4095, static)
      d.alpha_thresh = p->alpha_thresh;
      if (p->uv_mode == 3) {
        d.uv_fmt_b = p->el_tc1.fmt;
        d.uv_off_b = p->el_tc1.off;
        d.atlas_off = p->atlas_off;
        d.atlas_rows = p->atlas_rows;
      }
      uint64_t tab = p->tex_root >= 0 ? s->tab[p->tex_root & 31] : 0;
      bool resolved = false;
      if (tab && s->heap_gpu0 && tab >= s->heap_gpu0 && g_srv_inc) {
        uint64_t idx = (tab - s->heap_gpu0) / g_srv_inc + p->tex_off;
        if (idx < s->heap_num) {
          d.alpha_idx = int32_t(idx);
          d.alpha_heap = s->heap_gpu0;
          SrvInfo si;
          if (srv_lookup(s->heap_cpu0 + idx * g_srv_inc, si) && si.res &&
              si.dim == D3D12_SRV_DIMENSION_TEXTURE2D) {
            d.alpha_res = si.res;
            d.alpha_fmt = si.fmt;
            d.alpha_mip0 = si.mip0;
            d.alpha_mips = si.mips;
            resolved = true;
          }
        }
      }
      (resolved ? g_alpha_ok : g_alpha_miss).fetch_add(1, std::memory_order_relaxed);
      if (resolved) {
        static std::atomic<int> alogged{0};
        if (alogged.fetch_add(1, std::memory_order_relaxed) < 12) {
          D3D12_RESOURCE_DESC rd = d.alpha_res->GetDesc();
          TB_LOG("rt capture: alpha draw ps %016llx root %d+%u tab %#llx heap %#llx/%u idx %d res %p %llux%u fmt %u/%u",
                 (unsigned long long)d.ps, p->tex_root, p->tex_off, (unsigned long long)tab,
                 (unsigned long long)s->heap_gpu0, s->heap_num, d.alpha_idx, (void*)d.alpha_res,
                 (unsigned long long)rd.Width, rd.Height, unsigned(rd.Format), d.alpha_fmt);
        }
      }
    }
  }
  if (p->cls == DC_GBUFFER) {
    d.rt0 = s->rt[0];
    d.rt1 = s->rt[1];
  }
  s->draws.push_back(d);
  g_draws_kept.fetch_add(1, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
  HRESULT hr = o_reset(l, a, p);
  t_cur = l;
  if (ListSt* s = g_scene.load(std::memory_order_relaxed) ? list_st(l) : g_lists_st.find(l)) {
    s->draws.clear();
    s->trans.clear();
    s->pso = p ? g_psoinfo.find(p) : nullptr;
    s->viewport = s->depth_view = 0;
    s->rt[0] = s->rt[1] = nullptr;
    memset(s->tab, 0, sizeof s->tab);  // Reset clears the root arguments and bound heaps
    s->heap_cpu0 = s->heap_gpu0 = 0;
    s->heap_num = 0;
  }
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts.erase(l);
    ev("L %p reset tid=%lu pso=%u", (void*)l, GetCurrentThreadId(), pso_id(p));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_close(ID3D12GraphicsCommandList* l) {
  if (t_cur == l) t_cur = nullptr;
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("L %p close tid=%lu", (void*)l, GetCurrentThreadId());
  }
  return o_close(l);
}

void STDMETHODCALLTYPE hk_setPso(ID3D12GraphicsCommandList* l, ID3D12PipelineState* p) {
  ListSt* sp = g_spot.load() ? list_st(l) : nullptr;
  if (sp) {
    sp->raw_pso = p;
    // Swap off ([dev] rt_spot_on = 0), or no TLAS this frame for the ray-query one: the original
    // pipeline (shadow atlas).
    std::lock_guard lk(g_mu);
    auto it = g_spot_pso.find(p);
    if (it != g_spot_pso.end() && (!g_spot_on.load() || (g_spot.load() == 2 && !scene_spot_ready()))) {
      p = it->second;
      sp->raw_pso = p;
      g_spot_fallback++;
    }
  }
  o_setPso(l, p);
  t_cur = l;
  if (g_scene.load(std::memory_order_relaxed)) list_st(l)->pso = p ? g_psoinfo.find(p) : nullptr;
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("P %p %u", (void*)l, pso_id(p));
  }
}

void STDMETHODCALLTYPE hk_viewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* v) {
  o_viewports(l, n, v);
  if (g_scene.load(std::memory_order_relaxed)) list_st(l)->viewport = n == 1 && v ? fnv(v, sizeof *v) : 0;
}

void STDMETHODCALLTYPE hk_topo(ID3D12GraphicsCommandList* l, D3D12_PRIMITIVE_TOPOLOGY t) {
  o_topo(l, t);
  if (g_scene.load(std::memory_order_relaxed)) list_st(l)->topo = int(t);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("T %p %d", (void*)l, int(t));
  }
}

void STDMETHODCALLTYPE hk_setHeaps(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* hs) {
  o_setHeaps(l, n, hs);
  if (!g_scene.load(std::memory_order_relaxed)) return;
  ListSt* s = list_st(l);
  s->heap_cpu0 = s->heap_gpu0 = 0;
  s->heap_num = 0;
  for (UINT i = 0; i < n && hs; i++) {
    ID3D12DescriptorHeap* h = hs[i];
    if (!h) continue;
    D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
      continue;
    HeapEnds e;
    {
      std::lock_guard lk(g_mu);
      auto it = g_dheaps.find(h);
      if (it != g_dheaps.end()) {
        e = it->second;
      } else {
        e = {h->GetCPUDescriptorHandleForHeapStart().ptr, h->GetGPUDescriptorHandleForHeapStart().ptr,
             d.NumDescriptors};
        g_dheaps[h] = e;
      }
    }
    s->heap_cpu0 = e.cpu0;
    s->heap_gpu0 = e.gpu0;
    s->heap_num = e.num;
  }
}

void spot_bind(ID3D12GraphicsCommandList* l) {
  if (g_spot.load() != 2) return;
  ListSt* s = g_lists_st.find(l);
  if (!s || !s->raw_pso) return;
  int base;
  {
    std::lock_guard lk(g_mu);
    auto it = g_spot_base.find(s->raw_pso);
    if (it == g_spot_base.end()) return;
    base = it->second;
  }
  uint64_t tlas = 0, cb = 0, alpha = 0;
  if (base < 0 || !scene_spot_bind(&tlas, &cb, &alpha)) return;
  o_gSrv(l, UINT(base), tlas);
  o_gCbv(l, UINT(base + 1), cb);
  o_gSrv(l, UINT(base + 2), alpha);
  if (s->heap_gpu0) {
    D3D12_GPU_DESCRIPTOR_HANDLE h{s->heap_gpu0};  // t0 space10: the game's heap, by heap index
    o_gTab(l, UINT(base + 3), h);
  }
  g_spot_draws++;
}

void STDMETHODCALLTYPE hk_drawI(ID3D12GraphicsCommandList* l, UINT v, UINT i, UINT sv, UINT si) {
  spot_bind(l);
  o_drawI(l, v, i, sv, si);
  t_cur = l;
  if (g_scene.load(std::memory_order_relaxed)) {
    ListSt* s = list_st(l);
    PsoInfo* p = s->pso;
    if (p && (p->cls == DC_GBUFFER || p->cls == DC_SHADOW)) g_draws_nonidx.fetch_add(1, std::memory_order_relaxed);
    if (p && p->cls == DC_SUN && p->ready.load(std::memory_order_acquire) == 1 && s->cbv[p->cb_root & 31]) {
      DrawRec d;
      d.cls = DC_SUN;
      d.cb = s->cbv[p->cb_root & 31];
      d.l2v = p->l2v;
      d.ps = p->ps;
      s->draws.push_back(d);
    }
  }
  if (on()) {
    std::lock_guard lk(g_mu);
    char c[96];
    counts_line(l, c, sizeof c);
    ev("D %p %u %u %u %u %s", (void*)l, v, i, sv, si, c);
  }
}

void STDMETHODCALLTYPE hk_drawII(ID3D12GraphicsCommandList* l, UINT ic, UINT n, UINT si, INT bv, UINT sinst) {
  spot_bind(l);
  o_drawII(l, ic, n, si, bv, sinst);
  t_cur = l;
  if (g_scene.load(std::memory_order_relaxed)) scene_draw(l, ic, n, si, bv);
  if (on()) {
    std::lock_guard lk(g_mu);
    char c[96];
    counts_line(l, c, sizeof c);
    ev("X %p %u %u %u %d %u %s", (void*)l, ic, n, si, bv, sinst, c);
  }
}

void STDMETHODCALLTYPE hk_dispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
  o_dispatch(l, x, y, z);
  t_cur = l;
  if (on()) {
    std::lock_guard lk(g_mu);
    char c[96];
    counts_line(l, c, sizeof c);
    ev("K %p %u %u %u %s", (void*)l, x, y, z, c);
  }
}

void STDMETHODCALLTYPE hk_barrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
  o_barrier(l, n, b);
  if (!b) return;
  ListSt* s = g_scene.load(std::memory_order_relaxed) ? list_st(l) : nullptr;
  std::lock_guard lk(g_mu);
  unsigned tr = 0, uav = 0;
  for (UINT i = 0; i < n; i++) {
    if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      g_state[b[i].Transition.pResource] = unsigned(b[i].Transition.StateAfter);
      if (s && (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0)
        s->trans.emplace_back(b[i].Transition.pResource, unsigned(b[i].Transition.StateAfter));
      tr++;
    } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      uav++;
    }
  }
  if (on()) ev("BAR %p %u %u", (void*)l, tr, uav);
}

#define ROOT_HOOK(name, orig, field, T, letter)                                    \
  void STDMETHODCALLTYPE name(ID3D12GraphicsCommandList* l, UINT i, T v) {         \
    orig(l, i, v);                                                                 \
    if (on()) {                                                                    \
      std::lock_guard lk(g_mu);                                                    \
      g_counts[l].field++;                                                         \
    }                                                                              \
  }
void STDMETHODCALLTYPE hk_gCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) {
  o_gCbv(l, i, v);
  if (g_scene.load(std::memory_order_relaxed) && i < 32) list_st(l)->cbv[i] = v;
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].cbv++;
  }
}
ROOT_HOOK(hk_gSrv, o_gSrv, srv, D3D12_GPU_VIRTUAL_ADDRESS, "S")
ROOT_HOOK(hk_gUav, o_gUav, uav, D3D12_GPU_VIRTUAL_ADDRESS, "U")
ROOT_HOOK(hk_cCbv, o_cCbv, cbv, D3D12_GPU_VIRTUAL_ADDRESS, "C")
ROOT_HOOK(hk_cSrv, o_cSrv, srv, D3D12_GPU_VIRTUAL_ADDRESS, "S")
ROOT_HOOK(hk_cUav, o_cUav, uav, D3D12_GPU_VIRTUAL_ADDRESS, "U")
void STDMETHODCALLTYPE hk_gTab(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE v) {
  o_gTab(l, i, v);
  if (g_scene.load(std::memory_order_relaxed) && i < 32) list_st(l)->tab[i] = v.ptr;
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].tab++;
  }
}
ROOT_HOOK(hk_cTab, o_cTab, tab, D3D12_GPU_DESCRIPTOR_HANDLE, "T")
#undef ROOT_HOOK

void STDMETHODCALLTYPE hk_g32s(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT off) {
  o_g32s(l, i, n, d, off);
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].c32++;
  }
}
void STDMETHODCALLTYPE hk_c32s(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT off) {
  o_c32s(l, i, n, d, off);
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].c32++;
  }
}
void STDMETHODCALLTYPE hk_g32(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT off) {
  o_g32(l, i, v, off);
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].c32++;
  }
}
void STDMETHODCALLTYPE hk_c32(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT off) {
  o_c32(l, i, v, off);
  if (on()) {
    std::lock_guard lk(g_mu);
    g_counts[l].c32++;
  }
}

void STDMETHODCALLTYPE hk_ib(ID3D12GraphicsCommandList* l, const D3D12_INDEX_BUFFER_VIEW* v) {
  o_ib(l, v);
  if (g_scene.load(std::memory_order_relaxed)) {
    ListSt* s = list_st(l);
    if (v) s->ib = *v;
    else s->ib = {};
  }
  if (on()) {
    std::lock_guard lk(g_mu);
    if (v) ev("I %p %s %u %d", (void*)l, va_str(v->BufferLocation).c_str(), v->SizeInBytes, int(v->Format));
    else ev("I %p -", (void*)l);
  }
}

void STDMETHODCALLTYPE hk_vb(ID3D12GraphicsCommandList* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
  o_vb(l, start, n, v);
  if (g_scene.load(std::memory_order_relaxed)) {
    ListSt* s = list_st(l);
    for (UINT i = 0; i < n && start + i < 8; i++) s->vb[start + i] = v ? v[i] : D3D12_VERTEX_BUFFER_VIEW{};
  }
  if (on()) {
    std::lock_guard lk(g_mu);
    std::string s;
    for (UINT i = 0; i < n && v; i++) {
      char b[48];
      snprintf(b, sizeof b, ":%u:%u ", v[i].SizeInBytes, v[i].StrideInBytes);
      s += va_str(v[i].BufferLocation) + b;
    }
    ev("V %p %u %u %s", (void*)l, start, n, s.c_str());
  }
}

std::string view_str(std::unordered_map<SIZE_T, View>& m, SIZE_T h) {  // caller holds g_mu
  auto it = m.find(h);
  if (it == m.end()) return "?";
  describe(it->second.res);
  char b[48];
  snprintf(b, sizeof b, "%p:%d", (void*)it->second.res, it->second.fmt);
  return b;
}

UINT g_rtv_inc = 0;

void STDMETHODCALLTYPE hk_om(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* h, BOOL single,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
  o_om(l, n, h, single, ds);
  t_cur = l;
  if (g_scene.load(std::memory_order_relaxed)) {
    ListSt* s = list_st(l);
    s->depth_view = ds ? ds->ptr : 0;
    s->rt[0] = s->rt[1] = nullptr;
    if (n == 3 && h) {  // the G-buffer's three targets: remember depth (0) and normals (1)
      std::lock_guard lk(g_mu);
      for (UINT i = 0; i < 2; i++) {
        auto it = g_rtv.find(single ? h[0].ptr + SIZE_T(i) * g_rtv_inc : h[i].ptr);
        s->rt[i] = it != g_rtv.end() ? it->second.res : nullptr;
      }
    }
  }
  if (on()) {
    std::lock_guard lk(g_mu);
    std::string s;
    for (UINT i = 0; i < n && h; i++) s += view_str(g_rtv, single ? h[0].ptr + SIZE_T(i) * g_rtv_inc : h[i].ptr) + ",";
    ev("O %p %u %s ds=%s", (void*)l, n, s.empty() ? "-" : s.c_str(), ds ? view_str(g_dsv, ds->ptr).c_str() : "-");
  }
}

void STDMETHODCALLTYPE hk_clearDS(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d,
                                  UINT8 s, UINT n, const D3D12_RECT* r) {
  o_clearDS(l, h, f, d, s, n, r);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("CD %p %s %d %g", (void*)l, view_str(g_dsv, h.ptr).c_str(), int(f), double(d));
  }
}

void STDMETHODCALLTYPE hk_clearRT(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT n,
                                  const D3D12_RECT* r) {
  o_clearRT(l, h, c, n, r);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("CR %p %s %g %g %g %g", (void*)l, view_str(g_rtv, h.ptr).c_str(), c ? c[0] : 0., c ? c[1] : 0., c ? c[2] : 0.,
       c ? c[3] : 0.);
  }
}

std::string event_text(UINT meta, const void* data, UINT size) {
  std::string s;
  if (!data) return s;
  if (meta == 0) {  // UTF-16 string
    auto* w = static_cast<const wchar_t*>(data);
    for (UINT i = 0; i < size / 2 && w[i]; i++) s += char(w[i] < 128 ? w[i] : '?');
  } else {  // ANSI (1) or the PIX blob (2): printable bytes
    auto* c = static_cast<const char*>(data);
    for (UINT i = 0; i < size; i++)
      if (c[i] >= 32 && c[i] < 127) s += c[i];
  }
  return s;
}

void STDMETHODCALLTYPE hk_beginEvent(ID3D12GraphicsCommandList* l, UINT meta, const void* data, UINT size) {
  o_beginEvent(l, meta, data, size);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("EV %p B %u %s", (void*)l, meta, event_text(meta, data, size).c_str());
  }
}
void STDMETHODCALLTYPE hk_setMarker(ID3D12GraphicsCommandList* l, UINT meta, const void* data, UINT size) {
  o_setMarker(l, meta, data, size);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("EV %p M %u %s", (void*)l, meta, event_text(meta, data, size).c_str());
  }
}
void STDMETHODCALLTYPE hk_endEvent(ID3D12GraphicsCommandList* l) {
  o_endEvent(l);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("EV %p E", (void*)l);
  }
}

void STDMETHODCALLTYPE hk_execIndirect(ID3D12GraphicsCommandList* l, ID3D12CommandSignature* s, UINT n, ID3D12Resource* a,
                                       UINT64 ao, ID3D12Resource* c, UINT64 co) {
  o_execIndirect(l, s, n, a, ao, c, co);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("XI %p sig=%p max=%u args=%p count=%p", (void*)l, (void*)s, n, (void*)a, (void*)c);
  }
}

void STDMETHODCALLTYPE hk_copyRes(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* s) {
  o_copyRes(l, d, s);
  if (on()) {
    std::lock_guard lk(g_mu);
    describe(d);
    describe(s);
    ev("CP %p %p <- %p", (void*)l, (void*)d, (void*)s);
  }
}
void STDMETHODCALLTYPE hk_copyTex(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
                                  const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) {
  o_copyTex(l, d, x, y, z, s, b);
  if (on() && d && s) {
    std::lock_guard lk(g_mu);
    describe(d->pResource);
    describe(s->pResource);
    ev("CT %p %p <- %p", (void*)l, (void*)d->pResource, (void*)s->pResource);
  }
}
void STDMETHODCALLTYPE hk_copyBuf(ID3D12GraphicsCommandList* l, ID3D12Resource* d, UINT64 doff, ID3D12Resource* s,
                                  UINT64 soff, UINT64 n) {
  o_copyBuf(l, d, doff, s, soff, n);
  if (g_scene.load(std::memory_order_relaxed) && d) scene_invalidate(d->GetGPUVirtualAddress() + doff, n);
  if (on()) {
    std::lock_guard lk(g_mu);
    ev("CB %p %p <- %p %llu", (void*)l, (void*)d, (void*)s, (unsigned long long)n);
  }
}

void STDMETHODCALLTYPE hk_exec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* ls) {
  if (on()) {
    std::lock_guard lk(g_mu);
    std::string s;
    for (UINT i = 0; i < n; i++) {
      char b[24];
      snprintf(b, sizeof b, " %p", (void*)ls[i]);
      s += b;
    }
    ev("E %p tid=%lu n=%u%s", (void*)q, GetCurrentThreadId(), n, s.c_str());
  }
  if (!g_scene.load(std::memory_order_relaxed)) {
    o_exec(q, n, ls);
    return;
  }
  // Scene capture: the draws these lists recorded join the scene in submission order, and their
  // transitions advance the GPU-order resource states.
  std::vector<DrawRec> draws;
  {
    std::lock_guard lk(g_mu);
    for (UINT i = 0; i < n; i++) {
      ListSt* st = g_lists_st.find(ls[i]);
      if (!st) continue;
      for (auto& [r, a] : st->trans) g_gpu_state[r] = a;
      st->trans.clear();
      if (draws.empty()) draws.swap(st->draws);
      else draws.insert(draws.end(), st->draws.begin(), st->draws.end());
      st->draws.clear();
    }
  }
  o_exec(q, n, ls);
  scene_after_exec(q, n, ls, draws);
}

ID3D12Device* g_dev = nullptr;

void flush_pso_log() {  // caller holds g_mu
  if (g_pso_log.empty()) return;
  std::string path = out_dir() + "\\rt-pso.txt";
  FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"ab");
  if (!f) return;
  fwrite(g_pso_log.data(), 1, g_pso_log.size(), f);
  fclose(f);
  g_pso_log.clear();
}

}  // namespace

std::string out_dir() {
  const char* d = getenv("QUIVER_LAB_RUN_DIR");
  if (d && *d) return d;
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path, n);
  p = p.substr(0, p.find_last_of(L"\\/"));
  return std::string(p.begin(), p.end());
}

void capture_early(bool full, int spot) {
  if (full) g_full = true;
  g_spot = spot;
  HMODULE m = GetModuleHandleW(L"d3d12.dll");
  void* fn = m ? (void*)GetProcAddress(m, "D3D12CreateDevice") : nullptr;
  if (!fn) {
    TB_LOG("rt: d3d12.dll / D3D12CreateDevice not found; device hooks wait for the swapchain");
    return;
  }
  if (hook_fn(fn, (void*)hk_createDevice, (void**)&o_createDevice, "D3D12CreateDevice"))
    TB_LOG("rt: D3D12CreateDevice hooked (capture on)");
}

void capture_device(ID3D12Device* dev) {
  if (!dev || g_dev_hooked.exchange(true)) return;
  g_dev = dev;  // not AddRef'd: the game's device lives for the process
  hook_batch_begin();
  int fails = !hook_slot(dev, 10, (void*)hk_gfx, (void**)&o_gfx, "CreateGraphicsPipelineState");
  fails += !hook_slot(dev, 16, (void*)hk_rs, (void**)&o_rs, "CreateRootSignature");
  fails += !hook_slot(dev, 11, (void*)hk_cs, (void**)&o_cs, "CreateComputePipelineState");
  fails += !hook_slot(dev, 18, (void*)hk_srvv, (void**)&o_srvv, "CreateShaderResourceView");
  fails += !hook_slot(dev, 23, (void*)hk_copyDesc, (void**)&o_copyDesc, "CopyDescriptors");
  fails += !hook_slot(dev, 24, (void*)hk_copyDescSimple, (void**)&o_copyDescSimple, "CopyDescriptorsSimple");
  fails += !hook_slot(dev, 20, (void*)hk_rtv, (void**)&o_rtv, "CreateRenderTargetView");
  fails += !hook_slot(dev, 21, (void*)hk_dsv, (void**)&o_dsv, "CreateDepthStencilView");
  fails += !hook_slot(dev, 27, (void*)hk_committed, (void**)&o_committed, "CreateCommittedResource");
  fails += !hook_slot(dev, 28, (void*)hk_heap, (void**)&o_heap, "CreateHeap");
  fails += !hook_slot(dev, 29, (void*)hk_placed, (void**)&o_placed, "CreatePlacedResource");
  ID3D12Device2* d2 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device2), (void**)&d2)) && d2) {
    fails += !hook_slot(d2, 44, (void*)hk_lib, (void**)&o_lib, "CreatePipelineLibrary");
    fails += !hook_slot(d2, 47, (void*)hk_stream, (void**)&o_stream, "CreatePipelineState");
    d2->Release();
  }
  g_rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g_srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  hook_batch_end();
  bool ok = fails == 0;
  TB_LOG("rt: device %p hooks %s", (void*)dev, ok ? "installed" : "PARTLY FAILED");
}

bool capture_on() { return g_dev_hooked; }

bool capture_lists() {
  if (g_lists_hooked) return true;
  if (!g_dev) return false;
  ID3D12CommandAllocator* a = nullptr;
  ID3D12GraphicsCommandList* l = nullptr;
  ID3D12CommandQueue* q = nullptr;
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  bool ok = SUCCEEDED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                    (void**)&a)) &&
            SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr,
                                               __uuidof(ID3D12GraphicsCommandList), (void**)&l)) &&
            SUCCEEDED(g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q));
  int fails = 0;
  if (ok) {
    hook_batch_begin();
    fails += !hook_slot(l, 9, (void*)hk_close, (void**)&o_close, "cl Close");
    fails += !hook_slot(l, 10, (void*)hk_reset, (void**)&o_reset, "cl Reset");
    fails += !hook_slot(l, 12, (void*)hk_drawI, (void**)&o_drawI, "cl DrawInstanced");
    fails += !hook_slot(l, 13, (void*)hk_drawII, (void**)&o_drawII, "cl DrawIndexedInstanced");
    fails += !hook_slot(l, 14, (void*)hk_dispatch, (void**)&o_dispatch, "cl Dispatch");
    fails += !hook_slot(l, 15, (void*)hk_copyBuf, (void**)&o_copyBuf, "cl CopyBufferRegion");
    fails += !hook_slot(l, 16, (void*)hk_copyTex, (void**)&o_copyTex, "cl CopyTextureRegion");
    fails += !hook_slot(l, 17, (void*)hk_copyRes, (void**)&o_copyRes, "cl CopyResource");
    fails += !hook_slot(l, 20, (void*)hk_topo, (void**)&o_topo, "cl IASetPrimitiveTopology");
    fails += !hook_slot(l, 21, (void*)hk_viewports, (void**)&o_viewports, "cl RSSetViewports");
    fails += !hook_slot(l, 25, (void*)hk_setPso, (void**)&o_setPso, "cl SetPipelineState");
    fails += !hook_slot(l, 26, (void*)hk_barrier, (void**)&o_barrier, "cl ResourceBarrier");
    fails += !hook_slot(l, 28, (void*)hk_setHeaps, (void**)&o_setHeaps, "cl SetDescriptorHeaps");
    fails += !hook_slot(l, 31, (void*)hk_cTab, (void**)&o_cTab, "cl SetComputeRootDescriptorTable");
    fails += !hook_slot(l, 32, (void*)hk_gTab, (void**)&o_gTab, "cl SetGraphicsRootDescriptorTable");
    fails += !hook_slot(l, 33, (void*)hk_c32, (void**)&o_c32, "cl SetComputeRoot32BitConstant");
    fails += !hook_slot(l, 34, (void*)hk_g32, (void**)&o_g32, "cl SetGraphicsRoot32BitConstant");
    fails += !hook_slot(l, 35, (void*)hk_c32s, (void**)&o_c32s, "cl SetComputeRoot32BitConstants");
    fails += !hook_slot(l, 36, (void*)hk_g32s, (void**)&o_g32s, "cl SetGraphicsRoot32BitConstants");
    fails += !hook_slot(l, 37, (void*)hk_cCbv, (void**)&o_cCbv, "cl SetComputeRootConstantBufferView");
    fails += !hook_slot(l, 38, (void*)hk_gCbv, (void**)&o_gCbv, "cl SetGraphicsRootConstantBufferView");
    fails += !hook_slot(l, 39, (void*)hk_cSrv, (void**)&o_cSrv, "cl SetComputeRootShaderResourceView");
    fails += !hook_slot(l, 40, (void*)hk_gSrv, (void**)&o_gSrv, "cl SetGraphicsRootShaderResourceView");
    fails += !hook_slot(l, 41, (void*)hk_cUav, (void**)&o_cUav, "cl SetComputeRootUnorderedAccessView");
    fails += !hook_slot(l, 42, (void*)hk_gUav, (void**)&o_gUav, "cl SetGraphicsRootUnorderedAccessView");
    fails += !hook_slot(l, 43, (void*)hk_ib, (void**)&o_ib, "cl IASetIndexBuffer");
    fails += !hook_slot(l, 44, (void*)hk_vb, (void**)&o_vb, "cl IASetVertexBuffers");
    fails += !hook_slot(l, 46, (void*)hk_om, (void**)&o_om, "cl OMSetRenderTargets");
    fails += !hook_slot(l, 47, (void*)hk_clearDS, (void**)&o_clearDS, "cl ClearDepthStencilView");
    fails += !hook_slot(l, 48, (void*)hk_clearRT, (void**)&o_clearRT, "cl ClearRenderTargetView");
    fails += !hook_slot(l, 56, (void*)hk_setMarker, (void**)&o_setMarker, "cl SetMarker");
    fails += !hook_slot(l, 57, (void*)hk_beginEvent, (void**)&o_beginEvent, "cl BeginEvent");
    fails += !hook_slot(l, 58, (void*)hk_endEvent, (void**)&o_endEvent, "cl EndEvent");
    fails += !hook_slot(l, 59, (void*)hk_execIndirect, (void**)&o_execIndirect, "cl ExecuteIndirect");
    fails += !hook_slot(q, 10, (void*)hk_exec, (void**)&o_exec, "queue ExecuteCommandLists");
    hook_batch_end();
    l->Close();
  }
  if (q) q->Release();
  if (l) l->Release();
  if (a) a->Release();
  if (!ok) {
    TB_LOG("rt: could not create a command list to hook");
    return false;
  }
  g_lists_hooked = true;
  TB_LOG("rt: command-list and queue hooks installed (%d failed)", fails);
  return true;
}

std::string capture_status() {
  std::lock_guard lk(g_mu);
  char b[512];
  snprintf(b, sizeof b,
           "{\"device_hooks\":%s,\"list_hooks\":%s,\"psos\":%llu,\"graphics\":%llu,\"compute\":%llu,\"stream\":%llu,"
           "\"pipeline_libraries\":%llu,\"psos_before_first_census\":%llu,\"shaders\":%zu,\"resources\":%zu,\"buffers\":%zu,"
           "\"census_frames_left\":%d}",
           g_dev_hooked ? "true" : "false", g_lists_hooked ? "true" : "false", (unsigned long long)g_pso_count,
           (unsigned long long)g_pso_graphics, (unsigned long long)g_pso_compute, (unsigned long long)g_pso_stream,
           (unsigned long long)g_pso_library, (unsigned long long)g_pso_before_census, g_shaders.size(), g_res.size(),
           g_bufs.size(), g_census.load());
  flush_pso_log();
  return b;
}

bool census_begin(int frames, const std::string& path) {
  if (!capture_lists() || frames <= 0) return false;
  std::lock_guard lk(g_mu);
  if (!g_pso_before_census) g_pso_before_census = g_pso_count;
  flush_pso_log();
  g_log.clear();
  g_described.clear();
  g_counts.clear();
  g_census_path = path;
  g_frame = 0;
  g_census_done = false;
  ev("# census frames=%d psos_so_far=%llu", frames, (unsigned long long)g_pso_count);
  g_census = frames + 1;  // starts at the next present
  return true;
}

bool census_active() { return on(); }
bool census_done() { return g_census_done; }

void census_present() {
  if (!on()) return;
  std::lock_guard lk(g_mu);
  int left = g_census.load() - 1;
  ev("F %u", g_frame++);
  if (left <= 0) {
    // Write PSO lines first (draws refer to them), then the events.
    flush_pso_log();
    FILE* f = _wfopen(std::wstring(g_census_path.begin(), g_census_path.end()).c_str(), L"wb");
    if (f) {
      fwrite(g_log.data(), 1, g_log.size(), f);
      fclose(f);
    }
    TB_LOG("rt: census written to %s (%zu bytes)", g_census_path.c_str(), g_log.size());
    g_log.clear();
    g_log.shrink_to_fit();
    g_census_done = true;
  }
  g_census = left > 0 ? left : 0;
}

void census_mark(const char* name, int phase, const void* arg) {
  if (!on()) return;
  std::lock_guard lk(g_mu);
  ev("M %s %d list=%p tid=%lu arg=%p", name, phase, (void*)t_cur, GetCurrentThreadId(), arg);
}

void census_name(const char* name, ID3D12Resource* r) {
  if (!on() || !r) return;
  std::lock_guard lk(g_mu);
  describe(r);
  ev("RN %s %p", name, (void*)r);
}

// The engine's pool render targets are rend::Texture2DD3D12 (0xd8 bytes; ctor 0x51d180, static):
// +0xa8 points to its rend::TextureD3D12 (0x488 bytes, ctor 0x4fc830), which holds the
// ID3D12Resource. The resource is found by scanning that object for a pointer this capture saw
// created (so it needs [dev] rt_capture from launch).
ID3D12Resource* engine_resource(const void* obj) {
  auto readable = [](const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
  };
  if (!readable(obj, 0xd8)) return nullptr;
  auto* tex = static_cast<void* const*>(static_cast<void* const*>(obj)[0xa8 / 8]);
  if (!readable(tex, 0x488)) return nullptr;
  std::lock_guard lk(g_mu);
  for (int i = 0; i < 0x488 / 8; i++) {
    auto* r = static_cast<ID3D12Resource*>(tex[i]);
    if (g_res.count(r)) return r;
  }
  return nullptr;
}

ID3D12GraphicsCommandList* cur_list() { return t_cur; }

bool scene_capture(bool on) {
  if (on && (!g_dev_hooked || !capture_lists())) return false;
  g_scene = on;
  return true;
}
bool scene_capture_on() { return g_scene; }
void spot_enable(bool on) { g_spot_on = on; }

const void* cpu_at(uint64_t va, size_t n) {
  std::lock_guard lk(g_mu);
  auto it = g_bufs.upper_bound(va);
  if (it == g_bufs.begin()) return nullptr;
  --it;
  if (va + n > it->first + it->second.size) return nullptr;
  if (it->second.heap != D3D12_HEAP_TYPE_UPLOAD && it->second.heap != D3D12_HEAP_TYPE_CUSTOM) return nullptr;
  ID3D12Resource* r = it->second.res;
  auto m = g_mapped.find(r);
  void* p = nullptr;
  if (m != g_mapped.end()) {
    p = m->second;
  } else {
    D3D12_RANGE none{0, 0};
    if (FAILED(r->Map(0, &none, &p))) p = nullptr;
    g_mapped[r] = p;  // never unmapped: the engine keeps its upload buffers mapped too
  }
  return p ? static_cast<const uint8_t*>(p) + (va - it->first) : nullptr;
}

bool buf_at(uint64_t va, ID3D12Resource** res, uint64_t* base, uint64_t* size, int* heap, unsigned* flags) {
  std::lock_guard lk(g_mu);
  auto it = g_bufs.upper_bound(va);
  if (it == g_bufs.begin()) return false;
  --it;
  if (va >= it->first + it->second.size) return false;
  if (res) *res = it->second.res;
  if (base) *base = it->first;
  if (size) *size = it->second.size;
  if (heap) *heap = it->second.heap;
  if (flags) *flags = it->second.flags;
  return true;
}

unsigned gpu_state(ID3D12Resource* r) {
  std::lock_guard lk(g_mu);
  auto it = g_gpu_state.find(r);
  if (it != g_gpu_state.end()) return it->second;
  auto c = g_res.find(r);
  return c != g_res.end() ? c->second.init_state : 0u;
}

void exec_lists(ID3D12CommandQueue* q, unsigned n, ID3D12CommandList* const* ls) {
  if (o_exec) o_exec(q, n, ls);
  else q->ExecuteCommandLists(n, ls);
}

std::string scene_capture_status() {
  char b[896];
  snprintf(b, sizeof b,
           "{\"on\":%s,\"draws_seen\":%llu,\"draws_kept\":%llu,\"skipped_instanced\":%llu,\"skipped_topology\":%llu,"
           "\"skipped_reflection_pending\":%llu,\"pso_reflected\":%llu,\"pso_rejected\":%llu,\"no_binding\":%llu,"
           "\"out_of_range\":%llu,\"non_indexed\":%llu,\"alpha_tex\":%llu,\"alpha_tex_miss\":%llu,\"spot_swaps\":%llu,\"spot_draws\":%llu,"
           "\"spot_fallback\":%llu}",
           g_scene ? "true" : "false", (unsigned long long)g_draws_seen.load(), (unsigned long long)g_draws_kept.load(),
           (unsigned long long)g_draws_inst.load(), (unsigned long long)g_draws_topo.load(),
           (unsigned long long)g_draws_pending.load(), (unsigned long long)g_refl_ok.load(),
           (unsigned long long)g_refl_rej.load(), (unsigned long long)g_draws_nobind.load(),
           (unsigned long long)g_draws_range.load(), (unsigned long long)g_draws_nonidx.load(), (unsigned long long)g_alpha_ok.load(),
           (unsigned long long)g_alpha_miss.load(), (unsigned long long)g_spot_swaps.load(),
           (unsigned long long)g_spot_draws.load(), (unsigned long long)g_spot_fallback.load());
  return b;
}

unsigned res_state(ID3D12Resource* r) {
  std::lock_guard lk(g_mu);
  auto it = g_state.find(r);
  return it == g_state.end() ? 0u : it->second;
}

std::string dump_shaders(const std::string& dir, const std::string& which, bool disasm) {
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> todo;
  {
    std::lock_guard lk(g_mu);
    if (which == "all") {
      for (auto& [h, b] : g_shaders) todo.emplace_back(h, b);
    } else {
      size_t p = 0;
      while (p < which.size()) {
        size_t e = which.find(',', p);
        if (e == std::string::npos) e = which.size();
        uint64_t h = strtoull(which.substr(p, e - p).c_str(), nullptr, 16);
        auto it = g_shaders.find(h);
        if (it != g_shaders.end()) todo.emplace_back(h, it->second);
        p = e + 1;
      }
    }
  }
  CreateDirectoryW(std::wstring(dir.begin(), dir.end()).c_str(), nullptr);
  size_t n = 0, nd = 0;
  for (auto& [h, b] : todo) {
    char name[64];
    snprintf(name, sizeof name, "\\%016llx", (unsigned long long)h);
    std::string base = dir + name;
    FILE* f = _wfopen(std::wstring(base.begin(), base.end()).append(L".dxil").c_str(), L"wb");
    if (f) {
      fwrite(b.data(), 1, b.size(), f);
      fclose(f);
      n++;
    }
    std::string txt;
    if (disasm && dxc_disassemble(b.data(), b.size(), txt)) {
      f = _wfopen(std::wstring(base.begin(), base.end()).append(L".ll").c_str(), L"wb");
      if (f) {
        fwrite(txt.data(), 1, txt.size(), f);
        fclose(f);
        nd++;
      }
    }
  }
  char r[160];
  snprintf(r, sizeof r, "{\"requested\":%zu,\"written\":%zu,\"disassembled\":%zu,\"dir\":%s}", todo.size(), n, nd,
           json_str(dir).c_str());
  return r;
}

}  // namespace tb::rt
