// Raytracing feasibility probe: lab verbs, engine pass markers, GPU timestamps around engine
// passes, and render-target grabs (rt.h, docs/raytracing.md). Dev only.
//
//   rt dxr                 device caps (raytracing tier, shader model, VRAM)
//   rt dxr test            BLAS/TLAS + ray query + RT pipeline on the game's device, sizes, rates
//   rt status              capture counters
//   rt census N [TAG]      N frames of D3D12 events -> <run>/rt-census-TAG.txt (needs rt_capture)
//   rt shaders H,H|all [disasm]   captured DXIL (+ disassembly) -> <run>/rt-shaders/
//   rt gpu on|off|status   GPU time of engine passes (timestamps recorded into the engine's lists)
//   rt grab NAME WHERE     copy an engine render target at a pass boundary -> <run>/rt-grab-*.raw
//   rt flag ssao|hbao [0|1]  force the SSAO / HBAO branch of the main pass (code patch, dev)
//   rt scene on|off|status|validate [MASK] [img] [noalpha]|budget N|casters 0|1|reset
//                          geometry capture, BLAS/TLAS and the ray passes (scene.cpp, trace.cpp)
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "features/rt/rt.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "features/graphics.h"
#include "features/rt/rt_internal.h"
#include "features/rt/rt_scene.h"

namespace tb::rt {
namespace {

std::atomic<IDXGISwapChain*> g_sc{nullptr};

ID3D12Device* device() {
  IDXGISwapChain* sc = g_sc.load();
  auto* q = sc ? static_cast<ID3D12CommandQueue*>(present_queue(sc)) : nullptr;
  ID3D12Device* d = nullptr;
  if (q) q->GetDevice(__uuidof(ID3D12Device), (void**)&d);
  return d;  // AddRef'd
}

bool bytes_ok(uintptr_t rva, const uint8_t* b, size_t n) { return !memcmp((void*)game_image().at(rva), b, n); }

// ---- engine passes -----------------------------------------------------------------------------
// RVAs from the Ghidra project (static), each prologue checked before hooking. docs/raytracing.md.

enum Pass { P_MAIN, P_SSAO, P_SSAO_APPLY, P_HBAO, P_HBAO_APPLY, P_SHADOWBUF, P_SUN, P_POST, P_COUNT };
const char* kPassName[P_COUNT] = {"mainpass", "ssao", "ssao_apply", "hbao", "hbao_apply", "shadowbuf", "sun", "post"};

using F1 = void (*)(void*);
using F2 = void (*)(void*, void*);
using F4 = void (*)(void*, void*, void*, void*);
using F6 = void (*)(void*, void*, void*, void*, void*, void*);
F6 o_main;
F1 o_ssao, o_ssaoApply, o_hbao, o_hbaoApply;
F2 o_shadowbuf, o_sun;
F4 o_post;

std::atomic<void*> g_renderer{nullptr};  // RendererX86, from the main pass

// Pool render targets: RendererX86 field -> pool name (renderWorld 0x5090b0, static).
struct PoolRt {
  const char* name;
  size_t off;
};
const PoolRt kPool[] = {{"pColorRT", 0x260},          {"pColorRT_HalfRes", 0x268}, {"pColorUpsampleRT", 0x270},
                        {"pNormalRT", 0x280},         {"pGlossinessRT", 0x298},    {"pLinearDepthRT", 0x2a0},
                        {"pLinearDepthRT_HalfRes", 0x2a8}, {"pShadowRT", 0x2b0},   {"pLightRT", 0x2b8},
                        {"pSpecularRT", 0x2d0},       {"pVolumetricLightRT", 0x2d8}, {"pMotionVectorsRT", 0x2e0},
                        {"pTAAHistoryRT0", 0x2f0},    {"pTAAHistoryRT1", 0x2f8}};

ID3D12Resource* pool_res(const char* name) {
  void* r = g_renderer.load();
  if (!r) return nullptr;
  for (auto& p : kPool)
    if (!strcmp(p.name, name)) return engine_resource(*reinterpret_cast<void**>(static_cast<uint8_t*>(r) + p.off));
  return nullptr;
}

// --- GPU timestamps ---
struct Timer {
  ID3D12QueryHeap* qh = nullptr;
  ID3D12Resource* rb = nullptr;
  UINT64 freq = 0;
  static constexpr int kRing = 8;
  int frame = 0;
  bool open[kRing][P_COUNT] = {};
  bool done[kRing][P_COUNT] = {};
  double sum[P_COUNT] = {}, mn[P_COUNT], mx[P_COUNT] = {};
  uint64_t n[P_COUNT] = {};
  uint64_t frames = 0;
};
std::mutex g_tmu;
Timer g_t;
std::atomic<bool> g_timing{false};

UINT q_index(int ring, int pass, int phase) { return UINT((ring * P_COUNT + pass) * 2 + phase); }

bool timer_init() {
  if (g_t.qh) return true;
  ID3D12Device* d = device();
  if (!d) return false;
  D3D12_QUERY_HEAP_DESC hd{};
  hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  hd.Count = Timer::kRing * P_COUNT * 2;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = hd.Count * 8;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bool ok = SUCCEEDED(d->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), (void**)&g_t.qh)) &&
            SUCCEEDED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 __uuidof(ID3D12Resource), (void**)&g_t.rb));
  auto* q = static_cast<ID3D12CommandQueue*>(present_queue(g_sc.load()));
  if (q) q->GetTimestampFrequency(&g_t.freq);
  d->Release();
  for (auto& v : g_t.mn) v = 1e9;
  return ok;
}

void time_mark(Pass p, int phase) {
  if (!g_timing.load(std::memory_order_relaxed)) return;
  ID3D12GraphicsCommandList* l = cur_list();
  if (!l) return;
  std::lock_guard lk(g_tmu);
  int r = g_t.frame % Timer::kRing;
  if (phase == 0) {
    if (g_t.open[r][p] || g_t.done[r][p]) return;  // first occurrence per frame only
    l->EndQuery(g_t.qh, D3D12_QUERY_TYPE_TIMESTAMP, q_index(r, p, 0));
    g_t.open[r][p] = true;
  } else if (g_t.open[r][p]) {
    l->EndQuery(g_t.qh, D3D12_QUERY_TYPE_TIMESTAMP, q_index(r, p, 1));
    l->ResolveQueryData(g_t.qh, D3D12_QUERY_TYPE_TIMESTAMP, q_index(r, p, 0), 2, g_t.rb, q_index(r, p, 0) * 8);
    g_t.open[r][p] = false;
    g_t.done[r][p] = true;
  }
}

void timer_present() {
  if (!g_timing.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(g_tmu);
  g_t.frame++;
  g_t.frames++;
  int r = (g_t.frame + 2) % Timer::kRing;  // six frames ago
  UINT64* p = nullptr;
  D3D12_RANGE rr{0, size_t(Timer::kRing * P_COUNT * 2 * 8)};
  if (FAILED(g_t.rb->Map(0, &rr, (void**)&p))) return;
  for (int i = 0; i < P_COUNT; i++) {
    if (!g_t.done[r][i]) continue;
    UINT64 a = p[q_index(r, i, 0)], b = p[q_index(r, i, 1)];
    if (b > a && g_t.freq) {
      double ms = double(b - a) * 1000.0 / double(g_t.freq);
      if (ms < 1000) {
        g_t.sum[i] += ms;
        g_t.n[i]++;
        if (ms < g_t.mn[i]) g_t.mn[i] = ms;
        if (ms > g_t.mx[i]) g_t.mx[i] = ms;
      }
    }
    g_t.done[r][i] = false;
    g_t.open[r][i] = false;
  }
  D3D12_RANGE none{0, 0};
  g_t.rb->Unmap(0, &none);
}

std::string timer_status() {
  std::lock_guard lk(g_tmu);
  std::string s = "{\"frames\":" + std::to_string(g_t.frames) + ",\"passes\":{";
  bool first = true;
  for (int i = 0; i < P_COUNT; i++) {
    if (!g_t.n[i]) continue;
    char b[160];
    snprintf(b, sizeof b, "%s\"%s\":{\"n\":%llu,\"mean_ms\":%.4f,\"min_ms\":%.4f,\"max_ms\":%.4f}", first ? "" : ",",
             kPassName[i], (unsigned long long)g_t.n[i], g_t.sum[i] / g_t.n[i], g_t.mn[i], g_t.mx[i]);
    s += b;
    first = false;
  }
  return s + "}}";
}

void timer_reset() {
  std::lock_guard lk(g_tmu);
  for (int i = 0; i < P_COUNT; i++) {
    g_t.sum[i] = 0;
    g_t.n[i] = 0;
    g_t.mn[i] = 1e9;
    g_t.mx[i] = 0;
  }
  g_t.frames = 0;
}

// --- render-target grabs ---
struct Grab {
  std::string name, path;
  int pass = -1, phase = 0;
  bool armed = false, recorded = false;
  int wait = 0;
  ID3D12Resource* rb = nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  D3D12_RESOURCE_DESC desc{};
  std::string result;
};
std::mutex g_gmu;
Grab g_grab;

void grab_mark(Pass p, int phase, void* obj) {
  std::lock_guard lk(g_gmu);
  if (!g_grab.armed || g_grab.recorded || g_grab.pass != int(p) || g_grab.phase != phase) return;
  ID3D12Resource* r = nullptr;
  if (g_grab.name == "hbao" && p == P_HBAO_APPLY && obj) r = engine_resource(*reinterpret_cast<void**>((uint8_t*)obj + 0x18));
  else if (g_grab.name == "ssao" && p == P_SSAO_APPLY && obj)
    r = engine_resource(*reinterpret_cast<void**>((uint8_t*)obj + 0x8));
  else r = pool_res(g_grab.name.c_str());
  ID3D12GraphicsCommandList* l = cur_list();
  if (!r || !l) {
    g_grab.result = r ? "no current command list" : "render target not found";
    g_grab.armed = false;
    return;
  }
  D3D12_RESOURCE_DESC d = r->GetDesc();
  if (d.SampleDesc.Count > 1 || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    g_grab.result = "multisampled or not 2D";
    g_grab.armed = false;
    return;
  }
  ID3D12Device* dev = nullptr;
  r->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
  UINT64 total = 0;
  d.MipLevels = 1;
  d.DepthOrArraySize = 1;
  dev->GetCopyableFootprints(&d, 0, 1, 0, &g_grab.fp, nullptr, nullptr, &total);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (g_grab.rb) g_grab.rb->Release();
  g_grab.rb = nullptr;
  dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                               __uuidof(ID3D12Resource), (void**)&g_grab.rb);
  dev->Release();
  if (!g_grab.rb) {
    g_grab.result = "readback allocation failed";
    g_grab.armed = false;
    return;
  }
  unsigned st = res_state(r);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = 0;
  b.Transition.StateBefore = D3D12_RESOURCE_STATES(st);
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = g_grab.rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = g_grab.fp;
  src.pResource = r;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
  g_grab.desc = d;
  g_grab.recorded = true;
  g_grab.wait = 6;
  char info[160];
  snprintf(info, sizeof info, "%ux%u fmt %d state %#x", unsigned(d.Width), d.Height, int(d.Format), st);
  g_grab.result = info;
}

void grab_present() {
  std::lock_guard lk(g_gmu);
  if (!g_grab.recorded || --g_grab.wait > 0) return;
  void* p = nullptr;
  D3D12_RANGE rr{0, size_t(g_grab.fp.Footprint.RowPitch) * g_grab.fp.Footprint.Height};
  if (SUCCEEDED(g_grab.rb->Map(0, &rr, &p))) {
    FILE* f = _wfopen(std::wstring(g_grab.path.begin(), g_grab.path.end()).c_str(), L"wb");
    if (f) {
      // Header: "TBRT", width, height, DXGI format, row pitch (u32 each), then the rows.
      uint32_t h[5] = {0x54524254u, uint32_t(g_grab.desc.Width), g_grab.desc.Height, uint32_t(g_grab.desc.Format),
                       g_grab.fp.Footprint.RowPitch};
      fwrite(h, 4, 5, f);
      fwrite(p, 1, rr.End, f);
      fclose(f);
    }
    D3D12_RANGE none{0, 0};
    g_grab.rb->Unmap(0, &none);
  }
  g_grab.rb->Release();
  g_grab.rb = nullptr;
  g_grab.recorded = false;
  g_grab.armed = false;
  g_grab.result += " -> " + g_grab.path;
  TB_LOG("rt grab: %s", g_grab.result.c_str());
}

void mark(Pass p, int phase, void* obj) {
  scene_mark(int(p), phase, obj);
  if (census_active()) census_mark(kPassName[p], phase, obj);
  if (phase == 0) grab_mark(p, 0, obj);
  time_mark(p, phase);
  if (phase == 1) grab_mark(p, 1, obj);
}

void name_pool(void* r) {
  if (!census_active() || !r) return;
  for (auto& p : kPool) {
    void* o = *reinterpret_cast<void**>(static_cast<uint8_t*>(r) + p.off);
    if (ID3D12Resource* res = engine_resource(o)) census_name(p.name, res);
  }
}

void hk_main(void* r, void* a, void* b, void* c, void* d, void* e) {
  g_renderer = r;
  name_pool(r);
  mark(P_MAIN, 0, r);
  o_main(r, a, b, c, d, e);
  mark(P_MAIN, 1, r);
}
void hk_ssao(void* o) {
  mark(P_SSAO, 0, o);
  o_ssao(o);
  mark(P_SSAO, 1, o);
}
void hk_ssaoApply(void* o) {
  if (census_active() && o) census_name("SSAO+8", engine_resource(*reinterpret_cast<void**>((uint8_t*)o + 8)));
  mark(P_SSAO_APPLY, 0, o);
  o_ssaoApply(o);
  mark(P_SSAO_APPLY, 1, o);
}
void hk_hbao(void* o) {
  mark(P_HBAO, 0, o);
  o_hbao(o);
  mark(P_HBAO, 1, o);
}
void hk_hbaoApply(void* o) {
  if (census_active() && o) census_name("g_sHBAOBuffer", engine_resource(*reinterpret_cast<void**>((uint8_t*)o + 0x18)));
  mark(P_HBAO_APPLY, 0, o);
  o_hbaoApply(o);
  mark(P_HBAO_APPLY, 1, o);
}
void hk_shadowbuf(void* o, void* light) {
  mark(P_SHADOWBUF, 0, light);
  o_shadowbuf(o, light);
  mark(P_SHADOWBUF, 1, light);
}
void hk_sun(void* o, void* light) {
  mark(P_SUN, 0, light);
  o_sun(o, light);
  mark(P_SUN, 1, light);
}
void hk_post(void* o, void* a, void* b, void* c) {
  mark(P_POST, 0, o);
  o_post(o, a, b, c);
  mark(P_POST, 1, o);
}

struct Hook {
  uintptr_t rva;
  const char* name;
  uint8_t sig[16];
  void* det;
  void** orig;
};
const Hook kHooks[] = {
    {0x506b20, "main pass", {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55},
     (void*)hk_main, (void**)&o_main},
    {0x536630, "SSAO render", {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48},
     (void*)hk_ssao, (void**)&o_ssao},
    {0x536580, "SSAO apply", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x1d, 0x4f, 0xd0, 0x86},
     (void*)hk_ssaoApply, (void**)&o_ssaoApply},
    {0x538160, "HBAO render", {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48},
     (void*)hk_hbao, (void**)&o_hbao},
    {0x537ef0, "HBAO apply", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48},
     (void*)hk_hbaoApply, (void**)&o_hbaoApply},
    {0x52d5c0, "shadow buffer", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57},
     (void*)hk_shadowbuf, (void**)&o_shadowbuf},
    {0x52cb40, "sun shadow resolve", {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57},
     (void*)hk_sun, (void**)&o_sun},
    {0x535050, "post (0x535050)", {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57},
     (void*)hk_post, (void**)&o_post},
};
std::atomic<bool> g_engine_hooked{false};
// Scene + ray passes, wanted by [graphics] ao = rtao or `rt scene on`; switched at present (game
// thread): it needs the device (hooks from launch), the list hooks and the engine markers.
std::atomic<int> g_want_scene{0};
int g_have_scene = 0;
bool g_scene_failed_logged = false;

bool engine_hooks() {
  if (g_engine_hooked) return true;
  for (auto& h : kHooks)
    if (!bytes_ok(h.rva, h.sig, 16)) {
      TB_LOG("rt: signature mismatch for %s at RVA %#llx; engine markers off", h.name, (unsigned long long)h.rva);
      return false;
    }
  hook_batch_begin();
  bool ok = true;
  for (auto& h : kHooks) ok = hook_fn((void*)game_image().at(h.rva), h.det, h.orig, h.name) && ok;
  hook_batch_end();
  g_engine_hooked = ok;
  return ok;
}

// ---- verbs -------------------------------------------------------------------------------------

std::string verb(const std::vector<std::string>& a, bool& ok) {
  std::string sub = a.empty() ? "status" : a[0];
  if (sub == "status") return capture_status();
  if (sub == "dxr") {
    ID3D12Device* d = device();
    if (!d) {
      ok = false;
      return json_str("no swapchain / device yet");
    }
    std::string r = a.size() > 1 && a[1] == "test" ? dxr_test(d) : dxr_caps(d);
    d->Release();
    return r;
  }
  if (sub == "census") {
    int n = a.size() > 1 ? atoi(a[1].c_str()) : 1;
    std::string tag = a.size() > 2 ? a[2] : "x";
    if (!capture_on()) {
      ok = false;
      return json_str("device capture is off: launch with [dev] rt_capture = 1");
    }
    engine_hooks();
    std::string path = out_dir() + "\\rt-census-" + tag + ".txt";
    if (!census_begin(n, path)) {
      ok = false;
      return json_str("census could not start (command-list hooks)");
    }
    for (int i = 0; i < 2000 && !census_done(); i++) Sleep(10);  // lab thread: wait for the frames
    return std::string("{\"path\":") + json_str(path) + ",\"done\":" + (census_done() ? "true" : "false") + "}";
  }
  if (sub == "shaders") {
    std::string which = a.size() > 1 ? a[1] : "all";
    bool dis = a.size() > 2 && a[2] == "disasm";
    return dump_shaders(out_dir() + "\\rt-shaders", which, dis);
  }
  if (sub == "gpu") {
    std::string m = a.size() > 1 ? a[1] : "status";
    if (m == "on") {
      if (!capture_on() || !capture_lists() || !engine_hooks() || !timer_init()) {
        ok = false;
        return json_str("needs [dev] rt_capture = 1 and the engine hooks");
      }
      timer_reset();
      g_timing = true;
    } else if (m == "off") {
      g_timing = false;
    } else if (m == "reset") {
      timer_reset();
    }
    return timer_status();
  }
  if (sub == "grab") {
    if (a.size() < 3) {
      ok = false;
      return json_str("usage: rt grab NAME PASS[:0|1] (NAME: a pool RT like pShadowRT, hbao, ssao)");
    }
    if (!capture_on() || !capture_lists() || !engine_hooks()) {
      ok = false;
      return json_str("needs [dev] rt_capture = 1");
    }
    std::string w = a[2];
    int phase = 0;
    if (auto c = w.find(':'); c != std::string::npos) {
      phase = atoi(w.substr(c + 1).c_str());
      w = w.substr(0, c);
    }
    int pass = -1;
    for (int i = 0; i < P_COUNT; i++)
      if (w == kPassName[i]) pass = i;
    if (pass < 0) {
      ok = false;
      return json_str("unknown pass " + w);
    }
    {
      std::lock_guard lk(g_gmu);
      g_grab.name = a[1];
      g_grab.pass = pass;
      g_grab.phase = phase;
      g_grab.path = out_dir() + "\\rt-grab-" + (a.size() > 3 ? a[3] : a[1] + "-" + w + "-" + std::to_string(phase)) + ".raw";
      g_grab.result.clear();
      g_grab.recorded = false;
      g_grab.armed = true;
    }
    for (int i = 0; i < 300; i++) {
      Sleep(10);
      std::lock_guard lk(g_gmu);
      if (!g_grab.armed) return json_str(g_grab.result);
    }
    std::lock_guard lk(g_gmu);
    g_grab.armed = false;
    return json_str("timeout: " + g_grab.result);
  }
  if (sub == "flag") {
    if (a.size() < 2 || (a[1] != "ssao" && a[1] != "hbao")) {
      ok = false;
      return json_str("usage: rt flag ssao|hbao [game|1|0]");
    }
    if (a.size() > 2 && !graphics_branch(a[1], a[2] == "game" ? -1 : atoi(a[2].c_str()) ? 1 : 0)) {
      ok = false;
      return json_str("signature mismatch");
    }
    if (a.size() > 2) Sleep(100);  // applied at the next present
    uintptr_t g = a[1] == "ssao" ? 0xd755e0 : 0x955659;
    return "{\"branch\":" + json_str(graphics_branch_state(a[1])) + ",\"global\":" +
           std::to_string(*reinterpret_cast<const uint8_t*>(game_image().at(g))) + "}";
  }
  if (sub == "scene") {
    std::string m = a.size() > 1 ? a[1] : "status";
    if (m == "on" || m == "off") {
      g_want_scene = m == "on" ? 1 : 0;
      for (int i = 0; i < 100 && g_have_scene != g_want_scene.load(); i++) Sleep(10);  // applied at present
      if (g_have_scene != g_want_scene.load()) ok = false;
      return scene_status();
    }
    if (m == "validate") {
      unsigned mask = a.size() > 2 ? unsigned(strtoul(a[2].c_str(), nullptr, 0)) : 0x0f;
      bool img = false, alpha = true;
      for (size_t i = 3; i < a.size(); i++) {
        if (a[i] == "img") img = true;
        if (a[i] == "noalpha") alpha = false;  // forced-opaque rays: the pre-alpha baseline
      }
      trace_validate_arm(mask, img, alpha);
      Sleep(400);  // traced at the next kick, read back kSlots submissions later
      return scene_status();
    }
    return scene_verb(m, a.size() > 2 ? a[2] : "");
  }
  ok = false;
  return json_str("usage: rt status | dxr [test] | census N [TAG] | shaders H,H|all [disasm] | gpu on|off|reset|status | "
                  "grab NAME PASS[:phase] [TAG] | flag ssao|hbao [game|1|0] | "
                  "scene on|off|status|validate [MASK] [img]|budget N|casters 0|1|reset");
}

}  // namespace

ID3D12Resource* pool_resource(const char* name) { return pool_res(name); }

namespace {

void apply_scene() {
  int want = g_want_scene.load();
  if (want == g_have_scene) return;
  if (!want) {
    scene_enable(false);
    g_have_scene = 0;
    return;
  }
  if (capture_on() && capture_lists() && engine_hooks() && scene_enable(true)) {
    g_have_scene = 1;
    g_scene_failed_logged = false;
  } else if (!g_scene_failed_logged) {
    g_scene_failed_logged = true;
    TB_LOG("rt: ray-traced passes unavailable (%s)", capture_on() ? "hooks failed" :
           "the D3D12 capture is installed at launch: start the game with [graphics] ao = rtao");
  }
}

}  // namespace

void set_rtao(bool on) { g_want_scene = on ? 1 : 0; }
bool rtao_active() { return g_have_scene == 1 && scene_enabled(); }

void register_feature() {
  lab_verb("rt", verb);
  cfg_listen("dev.", [](const std::string& id, const std::string& v) {
    if (id.rfind("dev.rtao_", 0) == 0 || id.rfind("dev.rt_sun", 0) == 0 || id == "dev.rt_alpha") trace_config(id, v);
    if (id == "dev.rt_spot_on") spot_enable(v != "0");
    else if (id.rfind("dev.rt_spot_", 0) == 0) scene_spot_config(id, v);
  });
  present_on([](IDXGISwapChain* sc, uint64_t) {
    g_sc = sc;
    apply_scene();
    census_present();
    timer_present();
    grab_present();
  });
}

void start() {
  // The D3D12 capture goes in at launch (it tracks resources and pipeline states as they are
  // created): for the census ([dev] rt_capture) and for the ray-traced passes ([graphics] ao =
  // rtao, or [dev] rt_scene = 1 for the lab verbs). Otherwise nothing is hooked.
  bool full = cfg_bool("dev.rt_capture", false);
  int spot = int(cfg_num("dev.rt_spot", 0));
  if (full || cfg_str("graphics.ao") == "rtao" || cfg_bool("dev.rt_scene", false) || spot)
    capture_early(full, spot);
}

}  // namespace tb::rt
