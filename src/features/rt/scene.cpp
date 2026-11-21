// Geometry capture and acceleration structures for the ray-traced passes (rt_scene.h,
// docs/raytracing.md "Geometry capture").
//
// Per game update, on the game thread, at the engine's mid-frame kick (the ExecuteCommandLists
// inside the main pass, before HBAO: every G-buffer and shadow-map list of the render is in it):
//  - the draws census.cpp recorded on those lists (indexed triangle lists on G-buffer / shadow
//    pipelines, R32G32B32 positions, $Globals g_mLocalToView + g_mViewToWorld) become TLAS
//    instances: world (relative to an anchor, the camera) = ViewToWorld * LocalToView of the
//    draw's own view, so shadow-map draws (light views) land in the same space;
//  - one BLAS per mesh (vertex range + index range), built from the game's own buffers: static
//    meshes fast-trace + compaction, cached; meshes whose vertex buffer is a UAV (the pre-skinned
//    output of the Skinning pass) fast-build + refit every update;
//  - the TLAS is built once per update and shared by both stereo eyes;
//  - all of it, and the ray passes (trace.cpp), go into one list of ours submitted right after
//    the kick on the game's queue, so in GPU order it runs after the G-buffer and before the
//    lighting.
// Alpha-tested draws (PS discard or alpha-to-coverage) are kept in their own instance mask bits
// so a pass can include or exclude them; their geometry is non-opaque and tested against its own
// texture by the ray passes (the alpha buffer and UV gather below; shaders/alpha.hlsli).
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <smmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/present.h"
#include "features/rt/rt_internal.h"
#include "features/rt/rt_scene.h"
#include "features/stereo/stereo.h"

namespace tb::rt {
namespace {

constexpr int kSlots = 4;  // submissions in flight

template <typename T>
void rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// ---- GPU memory ---------------------------------------------------------------------------------

ID3D12Device5* g_dev = nullptr;
ID3D12CommandQueue* g_q = nullptr;  // the game's queue (not ours: not AddRef'd)
ID3D12Fence* g_fence = nullptr;
uint64_t g_fv = 0;  // last value signalled
ID3D12CommandAllocator* g_alloc[kSlots] = {};
uint64_t g_slot_fv[kSlots] = {};
ID3D12GraphicsCommandList4* g_list = nullptr;
uint64_t g_submits = 0;

uint64_t done_fv() { return g_fence ? g_fence->GetCompletedValue() : 0; }

ID3D12Resource* make_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size < 256 ? 256 : size;
  d.Height = d.DepthOrArraySize = d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  ID3D12Resource* r = nullptr;
  if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource),
                                            (void**)&r)))
    return nullptr;
  return r;
}

// Sub-allocated acceleration-structure memory: chunks of `chunk` bytes, first fit, 256-byte
// aligned, freed ranges coalesced.
struct Alloc {
  int chunk = -1;
  uint64_t off = 0, size = 0, va = 0;
  explicit operator bool() const { return chunk >= 0; }
};
struct Pool {
  const char* name;
  uint64_t chunk;
  struct Chunk {
    ID3D12Resource* buf;
    uint64_t va, size;
    std::map<uint64_t, uint64_t> free;  // offset -> size
  };
  std::vector<Chunk> chunks;
  uint64_t used = 0, reserved = 0;
  Alloc alloc(uint64_t n) {
    n = (n + 255) & ~255ull;
    for (int pass = 0; pass < 2; pass++) {
      for (size_t c = 0; c < chunks.size(); c++) {
        auto& ch = chunks[c];
        for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
          if (it->second < n) continue;
          Alloc a;
          a.chunk = int(c);
          a.off = it->first;
          a.size = n;
          a.va = ch.va + a.off;
          uint64_t rest = it->second - n, at = it->first + n;
          ch.free.erase(it);
          if (rest) ch.free[at] = rest;
          used += n;
          return a;
        }
      }
      uint64_t sz = std::max(chunk, n);
      ID3D12Resource* b = make_buffer(sz, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
      if (!b) return {};
      chunks.push_back({b, b->GetGPUVirtualAddress(), sz, {{0, sz}}});
      reserved += sz;
    }
    return {};
  }
  void free(Alloc& a) {
    if (!a) return;
    auto& f = chunks[size_t(a.chunk)].free;
    uint64_t off = a.off, n = a.size;
    auto nx = f.lower_bound(off);
    if (nx != f.end() && off + n == nx->first) {
      n += nx->second;
      nx = f.erase(nx);
    }
    if (nx != f.begin()) {
      auto pv = std::prev(nx);
      if (pv->first + pv->second == off) {
        off = pv->first;
        n += pv->second;
        f.erase(pv);
      }
    }
    f[off] = n;
    used -= a.size;
    a = {};
  }
};
Pool g_final{"final", 32ull << 20};    // compacted static BLAS, dynamic BLAS
Pool g_staging{"staging", 32ull << 20};  // static BLAS before compaction

struct Deferred {
  uint64_t fv;
  Pool* pool;
  Alloc a;
};
std::vector<Deferred> g_deferred;
std::vector<std::pair<uint64_t, ID3D12Resource*>> g_deferred_res;

void free_later(Pool& p, Alloc& a) {
  if (a) g_deferred.push_back({g_fv + 1, &p, a});
  a = {};
}
void release_later(ID3D12Resource*& r) {
  if (r) g_deferred_res.emplace_back(g_fv + 1, r);
  r = nullptr;
}
void run_deferred() {
  uint64_t d = done_fv();
  for (size_t i = 0; i < g_deferred.size();) {
    if (g_deferred[i].fv <= d) {
      g_deferred[i].pool->free(g_deferred[i].a);
      g_deferred[i] = g_deferred.back();
      g_deferred.pop_back();
    } else {
      i++;
    }
  }
  for (size_t i = 0; i < g_deferred_res.size();) {
    if (g_deferred_res[i].first <= d) {
      g_deferred_res[i].second->Release();
      g_deferred_res[i] = g_deferred_res.back();
      g_deferred_res.pop_back();
    } else {
      i++;
    }
  }
}

// A growable GPU buffer (scratch, TLAS, upload rings).
struct GrowBuf {
  ID3D12Resource* r = nullptr;
  uint64_t size = 0;
  void* cpu = nullptr;
  bool ensure(uint64_t n, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    if (r && size >= n) return true;
    release_later(r);
    cpu = nullptr;
    uint64_t sz = std::max<uint64_t>(n + n / 2, 1 << 16);
    r = make_buffer(sz, heap, fl, st);
    if (!r) {
      size = 0;
      return false;
    }
    size = sz;
    if (heap == D3D12_HEAP_TYPE_UPLOAD || heap == D3D12_HEAP_TYPE_READBACK) {
      D3D12_RANGE none{0, 0};
      if (FAILED(r->Map(0, heap == D3D12_HEAP_TYPE_READBACK ? nullptr : &none, &cpu))) cpu = nullptr;
    }
    return true;
  }
  uint64_t va() const { return r ? r->GetGPUVirtualAddress() : 0; }
};

GrowBuf g_scratch;                 // BLAS build / refit scratch (one update's batch)
GrowBuf g_tlas, g_tlas_scratch;
GrowBuf g_inst[kSlots];            // instance descs per slot
GrowBuf g_post;                    // postbuild compacted sizes (UAV)
GrowBuf g_post_rb[kSlots];         // their readback per slot
// Per-update BLAS of the meshes whose vertices are CPU-written every update (upload-heap vertex
// buffers: the terrain, camera-relative, and the grass; live): one geometry per draw, its own
// transform, opaque (0) and alpha-tested (1) apart; fast build, rebuilt every update.
GrowBuf g_xf[kSlots];              // their 3x4 transforms
GrowBuf g_dyn_as[2], g_dyn_scratch;
struct DynGeo {
  DrawRec d;
  float x[12];
};
std::vector<DynGeo> g_dyn[2];

// ---- BLAS cache ---------------------------------------------------------------------------------

struct Key {
  uint64_t vb, ib;
  uint32_t stride, vcount, icount;
  uint8_t ifmt, vfmt;
  bool alpha = false;  // non-opaque geometry (the flag is baked into the BLAS: keyed apart)
  bool operator==(const Key& o) const {
    return vb == o.vb && ib == o.ib && stride == o.stride && vcount == o.vcount && icount == o.icount &&
           ifmt == o.ifmt && vfmt == o.vfmt && alpha == o.alpha;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = k.vb * 0x9e3779b97f4a7c15ull ^ (k.ib + 0x632be59bd9b4e019ull) * 0xbf58476d1ce4e5b9ull;
    h ^= (uint64_t(k.stride) << 40) ^ (uint64_t(k.vcount) << 20) ^ k.icount ^ (uint64_t(k.ifmt) << 56) ^
         (k.alpha ? 0x100000001b3ull : 0);
    return size_t(h ^ (h >> 31));
  }
};
enum BlasSt { B_NEW, B_BUILT, B_COMPACTING, B_READY, B_REJECTED };
struct Blas {
  Key key;
  uint64_t vb_view = 0;
  uint32_t tris = 0;
  bool dynamic = false;
  uint8_t st = B_NEW;
  Alloc as, staging;
  int post_slot = -1, post_index = -1;  // where its compacted size is read back
  uint64_t last_used = 0;
  uint64_t refit_frame = 0;
  uint64_t update_scratch = 0;
  uint64_t va() const { return as ? as.va : staging ? staging.va : 0; }
};
std::unordered_map<Key, Blas*, KeyHash> g_blas;
std::unordered_set<uint64_t> g_upload_vb;  // vertex-buffer views in upload heaps (skipped)
struct PendingCompact {
  Blas* b;
  int index;
};
std::vector<PendingCompact> g_post_pending[kSlots];  // static builds whose size readback is in slot s

std::mutex g_inv_mu;
std::vector<std::pair<uint64_t, uint64_t>> g_inv;  // invalidated [va, va+size)

// ---- frame state --------------------------------------------------------------------------------

std::atomic<bool> g_on{false};
std::vector<DrawRec> g_pending;  // draws executed since the last kick (game thread only... and
std::mutex g_pending_mu;         // whichever thread submits: under this lock)
thread_local int t_main = 0;     // 1 inside the main pass, before its kick; 2 after the kick
uint64_t g_last_update = ~0ull;
double g_anchor[3] = {};
uint64_t g_tlas_va = 0;
uint32_t g_tlas_instances = 0;

// The alpha path and instance motion of the current update (EyeInputs; shared by both eyes).
GrowBuf g_alpha_buf[kSlots];   // records + gathered UVs (default heap)
unsigned g_alpha_st[kSlots] = {};  // its tracked state per slot (0 = as created, UAV)
GrowBuf g_alpha_up[kSlots];    // records + gather jobs + groups (upload)
GrowBuf g_motion_buf[kSlots];  // per TLAS instance: previous * inverse(current), 3x4 (upload)
uint64_t g_alpha_va = 0, g_motion_va = 0;
int g_alpha_slot = -1;
struct PrevX {
  float x[12];
  double anchor[3];
};
std::unordered_map<uint64_t, PrevX> g_prev_x;  // last update's instance transforms, by identity
struct AlphaStats {
  uint32_t records = 0, jobs = 0, tex = 0, bufs = 0, fallback = 0;
  uint64_t uv_bytes = 0;
} g_ast;
// One TLAS instance of an update, for the alpha records and the motion buffer.
struct Blas;
struct InstMeta {
  Blas* b = nullptr;  // null: a per-update BLAS instance (identity motion)
  float x[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  uint32_t geo0 = 0, ngeo = 0;  // its geometries in the update's geo_src (record geo0 + g)
};

struct Stats {
  uint64_t frames = 0, builds = 0, build_tris = 0, refits = 0, compactions = 0, evictions = 0, invalidated = 0;
  uint32_t views_shown = 0, views_placed_shown = 0;
  uint64_t dyn_dropped = 0, dyn_clamped = 0, cam_proj_cb = 0;
  uint64_t skip_upload = 0, dyn_tris_shown = 0, dyn_tris_a_shown = 0;
  double t_draws = 0, t_build = 0, t_total = 0;
  uint64_t skipped_busy = 0, skipped_nocam = 0, rejected = 0, rej_unknown = 0, rej_upload = 0, rej_range = 0;
  uint32_t last_draws_g = 0, last_draws_s = 0, last_inst = 0, last_inst_g = 0, last_inst_s = 0, last_dup = 0;
  uint32_t last_views = 0, last_views_placed = 0;
  uint32_t last_pending_blas = 0, last_builds = 0, last_refits = 0, last_dyn_geos = 0;
  uint64_t last_dyn_tris = 0, last_dyn_tris_a = 0;
  uint64_t last_tris_g = 0, last_tris_s = 0;
  double cpu_ms_sum = 0;
  uint64_t cpu_n = 0;
} g_st;

bool gpu_init(ID3D12CommandQueue* q) {
  if (g_dev) return true;
  ID3D12Device5* d = nullptr;
  if (FAILED(q->GetDevice(__uuidof(ID3D12Device5), (void**)&d)) || !d) {
    TB_LOG("rt scene: no ID3D12Device5");
    return false;
  }
  g_dev = d;
  g_q = q;
  bool ok = SUCCEEDED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence));
  for (int i = 0; i < kSlots && ok; i++)
    ok = SUCCEEDED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                 (void**)&g_alloc[i]));
  if (ok)
    ok = SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                            __uuidof(ID3D12GraphicsCommandList4), (void**)&g_list));
  if (ok) g_list->Close();
  std::string err;
  if (ok && !trace_init(g_dev, err)) {
    TB_LOG("rt scene: trace passes unavailable: %s", err.c_str());
    ok = false;
  }
  if (!ok) {
    TB_LOG("rt scene: GPU objects could not be created; ray-traced passes off");
    g_on = false;
  }
  return ok;
}

// $Globals reads: the draws' constant buffers are in the engine's upload rings, still intact at
// the kick (the CPU wrote them before recording the draw; the ring wraps frames later).
struct CbCache {
  uint64_t base = 0, end = 0;
  const uint8_t* cpu = nullptr;
  const void* at(uint64_t va, size_t n) {
    if (va >= base && va + n <= end && cpu) return cpu + (va - base);
    ID3D12Resource* r;
    uint64_t b, sz;
    int heap;
    if (!buf_at(va, &r, &b, &sz, &heap, nullptr)) return nullptr;
    const void* p = cpu_at(b, 1);
    if (!p) return nullptr;
    base = b;
    end = b + sz;
    cpu = static_cast<const uint8_t*>(p);
    return va + n <= end ? cpu + (va - base) : nullptr;
  }
  // The upload heap is write-combined (uncached) memory: ordinary loads stall on every access
  // (measured: 37 ms per update for ~4000 draws). Streaming loads (MOVNTDQA) fetch whole lines.
  // `va` must be 16-byte aligned (constant buffers are 256-aligned, matrices 16).
  __attribute__((target("sse4.1"))) bool read(uint64_t va, void* dst, size_t n) {
    const void* p = at(va, n);
    if (!p || (uintptr_t(p) & 15) || (n & 15)) return false;
    auto* s = static_cast<__m128i*>(const_cast<void*>(p));
    auto* d = static_cast<__m128i*>(dst);
    for (size_t i = 0; i < n / 16; i++) _mm_storeu_si128(d + i, _mm_stream_load_si128(s + i));
    return true;
  }
} g_cbc;

// out (3x4, row-major, the DXR instance layout) = a (3x4) * b (3x4), both affine, in doubles,
// translation minus the anchor.
void affine_mul(const double* a, const float* b, float* out, const double* anchor) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++) {
      double v = 0;
      for (int k = 0; k < 3; k++) v += a[r * 4 + k] * double(b[k * 4 + c]);
      if (c == 3) v += a[r * 4 + 3] - anchor[r];
      out[r * 4 + c] = float(v);
    }
}

// The camera's view-to-world (3x4, column vectors, absolute world, doubles) of the update.
double g_cam_v2w[12];

uint64_t qkey(const float* t, uint32_t tris) {
  // Instance identity for de-duplication: translation to 1 cm, rotation/scale rows to 1e-3.
  uint64_t h = 1469598103934665603ull ^ tris;
  for (int i = 0; i < 12; i++) {
    double s = (i % 4 == 3) ? 100.0 : 1000.0;
    int64_t q = llround(double(t[i]) * s);
    h = (h ^ uint64_t(q)) * 1099511628211ull;
  }
  return h;
}

void process_invalidations() {
  std::vector<std::pair<uint64_t, uint64_t>> inv;
  {
    std::lock_guard lk(g_inv_mu);
    inv.swap(g_inv);
  }
  if (inv.empty() || g_blas.empty()) return;
  std::sort(inv.begin(), inv.end());
  auto hit = [&](uint64_t a, uint64_t n) {
    auto it = std::upper_bound(inv.begin(), inv.end(), std::make_pair(a + n, uint64_t(0)));
    while (it != inv.begin()) {
      --it;
      if (it->first + it->second > a) return true;
      if (it->first + (1ull << 30) < a) break;  // ranges are far below (copies are < 1 GB)
    }
    return false;
  };
  for (auto it = g_blas.begin(); it != g_blas.end();) {
    Blas* b = it->second;
    uint64_t vbn = uint64_t(b->key.vcount) * b->key.stride, ibn = uint64_t(b->key.icount) * (b->key.ifmt == 42 ? 4 : 2);
    if (hit(b->key.vb, vbn) || hit(b->key.ib, ibn)) {
      free_later(g_final, b->as);
      free_later(g_staging, b->staging);
      if (b->post_slot >= 0) {
        auto& v = g_post_pending[b->post_slot];
        for (auto& p : v)
          if (p.b == b) p.b = nullptr;
      }
      delete b;
      it = g_blas.erase(it);
      g_st.invalidated++;
    } else {
      ++it;
    }
  }
}

void evict(uint64_t frame) {
  if (frame % 64) return;
  for (auto it = g_blas.begin(); it != g_blas.end();) {
    Blas* b = it->second;
    if (b->last_used + 900 < frame && b->st != B_COMPACTING) {
      free_later(g_final, b->as);
      free_later(g_staging, b->staging);
      delete b;
      it = g_blas.erase(it);
      g_st.evictions++;
    } else {
      ++it;
    }
  }
}

D3D12_RAYTRACING_GEOMETRY_DESC geo_of(const Blas& b) {
  D3D12_RAYTRACING_GEOMETRY_DESC g{};
  g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  g.Flags = b.key.alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;  // also for R32G32B32A32 (xyz read, stride)
  g.Triangles.VertexCount = b.key.vcount;
  g.Triangles.VertexBuffer.StartAddress = b.key.vb;
  g.Triangles.VertexBuffer.StrideInBytes = b.key.stride;
  g.Triangles.IndexFormat = DXGI_FORMAT(b.key.ifmt);
  g.Triangles.IndexCount = b.key.icount;
  g.Triangles.IndexBuffer = b.key.ib;
  return g;
}

void uav_barrier(ID3D12GraphicsCommandList4* l) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = nullptr;
  l->ResourceBarrier(1, &b);
}

// Config (dev tuning; [graphics] ao = rtao turns the whole thing on, graphics.cpp).
std::atomic<int> g_build_budget_tris{300000};
std::atomic<int> g_dyn_on{3};  // bit 0 opaque, bit 1 alpha-tested  // the per-update BLAS (upload-heap meshes)
std::atomic<int> g_dump{0};
std::atomic<uint64_t> g_dump_ps{0};  // only draws with this pixel shader (0: any)  // log this many instances of the next update (debug)
enum class CasterGrouping { Off, Projection, AtlasTile };
std::atomic<CasterGrouping> g_shadow_casters{CasterGrouping::Projection};

double now_ms() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

// The update's alpha buffer and instance motion (after the TLAS, same list): per geometry a
// record (alpha.hlsli), the alpha textures copied into the trace heap's slot region, raw views
// of the game's vertex / index / constant buffers for the UV gather (cs_gather), and per TLAS
// instance previous * inverse(current) for the temporal reprojection (cs_temporal). Instances
// are matched across updates by (BLAS, occurrence); the per-update BLAS instances keep identity
// motion (their transforms are per geometry). Unresolved geometries keep an opaque record.
void build_alpha(ID3D12GraphicsCommandList4* l, int slot, const std::vector<InstMeta>& meta,
                 const std::vector<DrawRec>& geos) {
  g_alpha_va = 0;
  g_motion_va = 0;
  g_alpha_slot = -1;
  g_ast = {};
  const uint32_t nrec = uint32_t(geos.size()), ninst = uint32_t(meta.size());
  g_ast.records = nrec;
  // Motion.
  std::unordered_map<uint64_t, PrevX> next;
  next.reserve(ninst * 2 + 16);
  if (ninst && g_motion_buf[slot].ensure(uint64_t(ninst) * 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_GENERIC_READ) &&
      g_motion_buf[slot].cpu) {
    float* out = static_cast<float*>(g_motion_buf[slot].cpu);
    std::unordered_map<Blas*, int> occ;
    for (uint32_t i = 0; i < ninst; i++) {
      const InstMeta& m = meta[i];
      float* o = out + size_t(i) * 12;
      static const float kI[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      memcpy(o, kI, sizeof kI);
      if (!m.b) continue;
      uint64_t key = (uint64_t(uintptr_t(m.b)) << 6) ^ uint64_t(occ[m.b]++) ^ 0x9e3779b97f4a7c15ull;
      PrevX px;
      memcpy(px.x, m.x, sizeof px.x);
      memcpy(px.anchor, g_anchor, sizeof px.anchor);
      next.emplace(key, px);
      auto it = g_prev_x.find(key);
      if (it == g_prev_x.end()) continue;
      const float* c = m.x;
      const float* p = it->second.x;
      double a[9] = {c[0], c[1], c[2], c[4], c[5], c[6], c[8], c[9], c[10]};
      double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                   a[2] * (a[3] * a[7] - a[4] * a[6]);
      if (!(std::fabs(det) > 1e-12)) continue;
      double ia[9] = {(a[4] * a[8] - a[5] * a[7]) / det,  (a[2] * a[7] - a[1] * a[8]) / det,
                      (a[1] * a[5] - a[2] * a[4]) / det,  (a[5] * a[6] - a[3] * a[8]) / det,
                      (a[0] * a[8] - a[2] * a[6]) / det,  (a[2] * a[3] - a[0] * a[5]) / det,
                      (a[3] * a[7] - a[4] * a[6]) / det,  (a[1] * a[6] - a[0] * a[7]) / det,
                      (a[0] * a[4] - a[1] * a[3]) / det};
      double tc[3] = {c[3], c[7], c[11]}, tp[3] = {p[3], p[7], p[11]};
      // M = prev * inverse(current), points in this update's anchor-relative world:
      // R = R_prev R_cur^-1, t = -R_prev (R_cur^-1 t_cur) + t_prev + (anchor_prev - anchor).
      double it3[3] = {ia[0] * tc[0] + ia[1] * tc[1] + ia[2] * tc[2],
                       ia[3] * tc[0] + ia[4] * tc[1] + ia[5] * tc[2],
                       ia[6] * tc[0] + ia[7] * tc[1] + ia[8] * tc[2]};
      for (int r = 0; r < 3; r++) {
        for (int cc = 0; cc < 3; cc++)
          o[r * 4 + cc] = float(p[r * 4] * ia[cc] + p[r * 4 + 1] * ia[3 + cc] + p[r * 4 + 2] * ia[6 + cc]);
        o[r * 4 + 3] = float(-(p[r * 4] * it3[0] + p[r * 4 + 1] * it3[1] + p[r * 4 + 2] * it3[2]) + tp[r] +
                             (it->second.anchor[r] - g_anchor[r]));
      }
    }
    g_motion_va = g_motion_buf[slot].va();
  }
  g_prev_x.swap(next);
  if (!nrec) return;
  // Alpha records and gather jobs.
  uint64_t tex_cpu = 0, buf_cpu = 0;
  unsigned inc = 0;
  if (!trace_alpha_heap(slot, &tex_cpu, &buf_cpu, &inc) || !inc) return;
  struct Job {
    uint32_t v[16];
  };
  std::vector<Job> jobs;
  std::vector<std::pair<uint32_t, uint32_t>> groups;
  std::vector<uint32_t> rec(size_t(nrec) * 4);
  for (uint32_t i = 0; i < nrec; i++) rec[size_t(i) * 4] = ~0u;  // opaque
  const uint32_t rec_bytes = nrec * 16, uv_base = (rec_bytes + 255) & ~255u;
  uint32_t uv_cur = uv_base;
  std::unordered_map<uint64_t, int> texmap;  // (resource, view) -> index in the mod's heap region
  struct BufSlot {
    int idx;
    uint64_t base;
  };
  std::unordered_map<ID3D12Resource*, BufSlot> bufmap;
  auto buf_slot = [&](uint64_t va, uint64_t* base) -> int {
    ID3D12Resource* res = nullptr;
    uint64_t b = 0, sz = 0;
    if (!buf_at(va, &res, &b, &sz, nullptr, nullptr) || !res) return -1;
    *base = b;
    auto it = bufmap.find(res);
    if (it != bufmap.end()) return it->second.idx;
    int idx = -1;
    // The gather reads the buffer as an SRV in compute, in whatever state the game's barriers
    // left it (the BLAS builds above already read the same buffers the same way, after the
    // list's opening UAV barrier; never transition the game's resources here).
    if (int(bufmap.size()) < kGatherBufs) {
      idx = int(bufmap.size());
      D3D12_SHADER_RESOURCE_VIEW_DESC d{};
      d.Format = DXGI_FORMAT_R32_TYPELESS;
      d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      d.Buffer.NumElements = UINT(sz / 4);
      d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
      D3D12_CPU_DESCRIPTOR_HANDLE h{size_t(buf_cpu + uint64_t(idx) * inc)};
      g_dev->CreateShaderResourceView(res, &d, h);
    }
    bufmap.emplace(res, BufSlot{idx, b});
    return idx;
  };
  for (uint32_t g = 0; g < nrec; g++) {
    const DrawRec& d = geos[g];
    uint32_t tris = d.icount / 3;
    if (!d.uv_mode || d.alpha_idx < 0 || !d.alpha_res || !tris) continue;
    uint64_t ib_base = 0, vb_base = 0, cb_base = 0;
    int ib_s = buf_slot(d.ib, &ib_base), vb_s = buf_slot(d.uv_vb, &vb_base);
    int cb_s = d.uv_mode == 3 ? buf_slot(d.cb, &cb_base) : 0;
    int tex_i = -1;
    uint64_t tkey = (uint64_t(uintptr_t(d.alpha_res)) * 0x9e3779b97f4a7c15ull) ^ d.alpha_fmt ^
                    (uint64_t(d.alpha_mip0) << 20) ^ (uint64_t(d.alpha_mips) << 36);
    auto ti = texmap.find(tkey);
    if (ti != texmap.end()) {
      tex_i = ti->second;
    } else if (int(texmap.size()) < kAlphaTex) {
      tex_i = int(texmap.size());
      D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
      vd.Format = DXGI_FORMAT(d.alpha_fmt);
      vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      vd.Texture2D.MostDetailedMip = d.alpha_mip0;
      vd.Texture2D.MipLevels = d.alpha_mips == 0xffff ? ~0u : d.alpha_mips;
      D3D12_CPU_DESCRIPTOR_HANDLE dst{size_t(tex_cpu + uint64_t(tex_i) * inc)};
      g_dev->CreateShaderResourceView(d.alpha_res, &vd, dst);
      texmap.emplace(tkey, tex_i);
    }
    if (ib_s < 0 || vb_s < 0 || cb_s < 0 || tex_i < 0 || d.ib - ib_base > 0xffffffffull ||
        d.uv_vb - vb_base > 0xffffffffull) {
      g_ast.fallback++;
      static std::atomic<int> flogged{0};
      if (flogged.fetch_add(1, std::memory_order_relaxed) < 6)
        TB_LOG("rt scene: alpha job skipped mode %d ib %d vb %d cb %d tex %d (ib %#llx vb %#llx)", d.uv_mode, ib_s, vb_s,
               cb_s, tex_i, (unsigned long long)d.ib, (unsigned long long)d.uv_vb);
      continue;
    }
    uint32_t job_i = uint32_t(jobs.size());
    Job j{};
    j.v[0] = uint32_t(ib_s);
    j.v[1] = uint32_t(d.ib - ib_base);
    j.v[2] = tris;
    j.v[3] = uv_cur;
    j.v[4] = uint32_t(vb_s);
    j.v[5] = uint32_t(d.uv_vb - vb_base);
    j.v[6] = d.uv_stride;
    j.v[7] = uint32_t(d.uv_mode) | (uint32_t(d.uv_fmt_a) << 8) | (uint32_t(d.uv_fmt_b) << 16) |
             (uint32_t(d.uv_comp_a) << 24);
    j.v[8] = uint32_t(d.uv_off_a) | (uint32_t(d.uv_off_b) << 16);
    memcpy(&j.v[9], &d.uv_scale, 4);
    j.v[10] = uint32_t(cb_s);
    j.v[11] = d.uv_mode == 3 ? uint32_t(d.cb - cb_base) + d.atlas_off : 0;
    j.v[12] = d.atlas_rows;
    j.v[13] = d.ifmt == 42 ? 4 : 2;
    jobs.push_back(j);
    for (uint32_t t0 = 0; t0 < tris; t0 += 64) groups.emplace_back(job_i, t0);
    rec[size_t(g) * 4 + 0] = uv_cur;
    rec[size_t(g) * 4 + 1] = uint32_t(tex_i);
    rec[size_t(g) * 4 + 2] = uint32_t(d.alpha_idx);
    memcpy(&rec[size_t(g) * 4 + 3], &d.alpha_thresh, 4);
    uv_cur += tris * 24;
  }
  g_ast.jobs = uint32_t(jobs.size());
  g_ast.tex = uint32_t(texmap.size());
  g_ast.bufs = uint32_t(bufmap.size());
  g_ast.uv_bytes = uv_cur > uv_base ? uv_cur - uv_base : 0;
  if (jobs.empty()) return;
  const uint64_t jobs_bytes = jobs.size() * 64, groups_bytes = groups.size() * 8;
  if (!g_alpha_up[slot].ensure(rec_bytes + jobs_bytes + groups_bytes, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ) ||
      !g_alpha_up[slot].cpu)
    return;
  auto* up = static_cast<uint8_t*>(g_alpha_up[slot].cpu);
  memcpy(up, rec.data(), rec_bytes);
  memcpy(up + rec_bytes, jobs.data(), jobs_bytes);
  memcpy(up + rec_bytes + jobs_bytes, groups.data(), groups_bytes);
  ID3D12Resource* old_r = g_alpha_buf[slot].r;
  if (!g_alpha_buf[slot].ensure(uv_cur, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    return;
  if (g_alpha_buf[slot].r != old_r) g_alpha_st[slot] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto trans = [&](unsigned to) {
    if (g_alpha_st[slot] == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_alpha_buf[slot].r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATES(g_alpha_st[slot]);
    b.Transition.StateAfter = D3D12_RESOURCE_STATES(to);
    l->ResourceBarrier(1, &b);
    g_alpha_st[slot] = to;
  };
  trans(D3D12_RESOURCE_STATE_COPY_DEST);
  l->CopyBufferRegion(g_alpha_buf[slot].r, 0, g_alpha_up[slot].r, 0, rec_bytes);
  trans(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  trace_gather(l, slot, g_alpha_up[slot].va() + rec_bytes, uint32_t(jobs_bytes), uint32_t(groups.size()),
               g_alpha_buf[slot].va());
  trace_mark(l, slot, 7);
  trans(D3D12_RESOURCE_STATE_GENERIC_READ);
  g_alpha_va = g_alpha_buf[slot].va();
  g_alpha_slot = slot;
}

// One update's acceleration structures: BLAS builds/refits/compactions, then the TLAS.
void build_scene(ID3D12GraphicsCommandList4* l, std::vector<DrawRec>& draws, int slot, uint64_t frame) {
  // Compactions whose sizes came back (this slot's previous use has completed).
  {
    auto& pend = g_post_pending[slot];
    const uint64_t* sizes = static_cast<const uint64_t*>(g_post_rb[slot].cpu);
    for (auto& p : pend) {
      Blas* b = p.b;
      if (!b || b->st != B_COMPACTING) continue;
      uint64_t sz = sizes ? sizes[p.index] : 0;
      b->post_slot = -1;
      if (!sz || sz > (1ull << 30)) {  // no size: keep the uncompacted one
        b->st = B_BUILT;
        continue;
      }
      Alloc a = g_final.alloc(sz);
      if (!a) {
        b->st = B_BUILT;
        continue;
      }
      l->CopyRaytracingAccelerationStructure(a.va, b->staging.va,
                                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
      b->as = a;
      free_later(g_staging, b->staging);
      b->st = B_READY;
      g_st.compactions++;
    }
    pend.clear();
  }

  double t0 = now_ms();
  // Instances.
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst;
  inst.reserve(draws.size());
  // Per instance, for the alpha records and the motion buffer: its geometries' draws (in
  // geometry order: one for a cached BLAS, the per-update BLAS's draws for dyn), its transform
  // and its identity across updates (the BLAS and its occurrence among that BLAS's instances).
  std::vector<InstMeta> meta;
  std::vector<DrawRec> geo_src;
  meta.reserve(draws.size());
  geo_src.reserve(draws.size());
  std::unordered_set<uint64_t> seen, at_xform;
  std::vector<Blas*> to_build, to_refit;
  uint64_t budget = uint64_t(g_build_budget_tris.load());
  uint64_t tris_g = 0, tris_s = 0;
  uint32_t ng = 0, ns = 0, dup = 0, dg = 0, dsh = 0, nblas_pending = 0;
  // Shadow-map draws are in light views (cascades, spot lights) whose placement is not in their
  // constants (only the rows a VS reads are written: g_mLocalToView, g_mViewToClip). Group them
  // by projection or, for the atlas experiment, by depth view and viewport. Place each view from
  // a mesh drawn once in the G-buffer and once in that view: World_gbuffer * inverse(LocalToView_light).
  std::vector<int> light_of(draws.size(), -1);
  std::vector<std::array<double, 12>> light_v2w;
  const CasterGrouping grouping = g_shadow_casters.load();
  if (grouping != CasterGrouping::Off) {
    struct G {
      int n = 0;
      std::array<double, 12> w{};
    };
    std::unordered_map<uint64_t, G> gm;
    auto mkey = [](const DrawRec& d) {
      return (d.vb * 0x9e3779b97f4a7c15ull) ^ (d.ib * 0xbf58476d1ce4e5b9ull) ^ d.icount;
    };
    for (auto& d : draws) {
      if (d.cls != DC_GBUFFER) continue;
      G& g = gm[mkey(d)];
      if (++g.n > 1) continue;
      float l2v[12];
      if (!g_cbc.read(d.cb + d.l2v, l2v, 48)) {
        g.n = 2;
        continue;
      }
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
          double v = c == 3 ? g_cam_v2w[r * 4 + 3] : 0;
          for (int k = 0; k < 3; k++) v += g_cam_v2w[r * 4 + k] * l2v[k * 4 + c];
          g.w[r * 4 + c] = v;
        }
    }
    std::unordered_map<uint64_t, int> views;  // group key -> light index
    struct V {
      std::unordered_map<uint64_t, int> count;
      std::vector<size_t> idx;
    };
    std::vector<V> vs;
    for (size_t i = 0; i < draws.size(); i++) {
      auto& d = draws[i];
      if (d.cls != DC_SHADOW) continue;
      uint64_t h = d.shadow_tile;
      if (grouping == CasterGrouping::AtlasTile) {
        if (!h) continue;
      } else {
        float proj[16];
        if (!g_cbc.read(d.cb + 128, proj, 64)) continue;
        h = 1469598103934665603ull;
        for (float f : proj) {
          uint32_t u;
          memcpy(&u, &f, 4);
          h = (h ^ u) * 1099511628211ull;
        }
      }
      auto [it, fresh] = views.emplace(h, int(vs.size()));
      if (fresh) vs.emplace_back();
      vs[it->second].count[mkey(d)]++;
      vs[it->second].idx.push_back(i);
    }
    for (auto& v : vs) {
      int li = -1;
      for (size_t i : v.idx) {
        auto& d = draws[i];
        uint64_t k = mkey(d);
        auto g = gm.find(k);
        if (g == gm.end() || g->second.n != 1 || v.count[k] != 1) continue;
        float l[12];
        if (!g_cbc.read(d.cb + d.l2v, l, 48)) continue;
        // inverse of the affine L (3x3 R, t): R^-1, -R^-1 t (general 3x3 inverse: scale allowed)
        double a[9] = {l[0], l[1], l[2], l[4], l[5], l[6], l[8], l[9], l[10]};
        double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
        if (!(std::fabs(det) > 1e-9)) continue;
        double ia[9] = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
                        (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                        (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
        double it_[3] = {-(ia[0] * l[3] + ia[1] * l[7] + ia[2] * l[11]), -(ia[3] * l[3] + ia[4] * l[7] + ia[5] * l[11]),
                         -(ia[6] * l[3] + ia[7] * l[7] + ia[8] * l[11])};
        std::array<double, 12> m{};
        const auto& w = g->second.w;
        for (int r = 0; r < 3; r++) {
          for (int c = 0; c < 3; c++) m[r * 4 + c] = w[r * 4] * ia[c] + w[r * 4 + 1] * ia[3 + c] + w[r * 4 + 2] * ia[6 + c];
          m[r * 4 + 3] = w[r * 4] * it_[0] + w[r * 4 + 1] * it_[1] + w[r * 4 + 2] * it_[2] + w[r * 4 + 3];
        }
        li = int(light_v2w.size());
        light_v2w.push_back(m);
        break;
      }
      g_st.last_views++;
      if (li < 0) continue;
      g_st.last_views_placed++;
      for (size_t i : v.idx) light_of[i] = li;
    }
  }
  // G-buffer draws first: a shadow-map draw of the same mesh at the same place is a duplicate.
  for (int pass = 0; pass < 2; pass++) {
    for (auto& d : draws) {
      size_t di = size_t(&d - draws.data());
      if (d.cls == DC_SUN || (pass == 0) != (d.cls == DC_GBUFFER)) continue;
      if (pass == 1 && grouping == CasterGrouping::Off) continue;
      (pass ? dsh : dg)++;
      // Only the $Globals rows a shader reads are written per draw (live: g_mViewToWorld at 64
      // holds stale ring data in G-buffer draws), so the view's placement comes from the engine's
      // View; shadow-map draws are in light views whose placement is not in their constants.
      if (pass == 1 && light_of[di] < 0) continue;
      float l2v[12];
      if (!g_cbc.read(d.cb + d.l2v, l2v, 48)) continue;
      float x[12];
      affine_mul(pass ? light_v2w[size_t(light_of[di])].data() : g_cam_v2w, l2v, x, g_anchor);
      if (g_dump.load() > 0 && (!g_dump_ps.load() || g_dump_ps.load() == d.ps)) {
        g_dump--;
        TB_LOG("rt dump: cls %d tris %u vb %#llx stride %u vcount %u ib %#llx fmt %d l2v@%u | l2v %.3f %.3f %.3f %.3f / "
               "%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f | v2w t %.2f %.2f %.2f | x t %.2f %.2f %.2f r0 %.3f %.3f %.3f",
               d.cls, d.icount / 3, (unsigned long long)d.vb, d.stride, d.vcount, (unsigned long long)d.ib, d.ifmt, d.l2v, l2v[0],
               l2v[1], l2v[2], l2v[3], l2v[4], l2v[5], l2v[6], l2v[7], l2v[8], l2v[9], l2v[10], l2v[11], g_cam_v2w[3], g_cam_v2w[7],
               g_cam_v2w[11], x[3], x[7], x[11], x[0], x[1], x[2]);
      }
      bool bad = false;
      for (float f : x)
        if (!std::isfinite(f) || std::fabs(f) > 1e6f) bad = true;
      if (bad) continue;
      uint32_t tris = d.icount / 3;
      uint64_t xk = qkey(x, tris);
      if (pass == 1 && at_xform.count(xk)) {
        dup++;
        continue;
      }
      Key k{d.vb, d.ib, d.stride, d.vcount, d.icount, d.ifmt, d.vfmt, (d.flags & DF_ALPHA) != 0};
      auto it = g_blas.find(k);
      Blas* b;
      if (it == g_blas.end() && g_upload_vb.count(d.vb_view)) {
        if (pass == 0) g_dyn[(d.flags & DF_ALPHA) ? 1 : 0].push_back({d, {}}), memcpy(g_dyn[(d.flags & DF_ALPHA) ? 1 : 0].back().x, x, sizeof x);
        g_st.skip_upload++;
        continue;
      }
      if (it == g_blas.end()) {
        b = new Blas;
        b->key = k;
        b->vb_view = d.vb_view;
        b->tris = tris;
        int heap = 0;
        unsigned flags = 0;
        ID3D12Resource* res = nullptr;
        uint64_t base = 0, size = 0;
        if (!buf_at(d.vb_view, &res, &base, &size, &heap, &flags)) {
          b->st = B_REJECTED;  // a vertex buffer the capture did not see created
          g_st.rej_unknown++;
        } else if (heap == D3D12_HEAP_TYPE_UPLOAD) {
          // CPU-written each frame (a ring: new addresses every update): not cached.
          g_st.rej_upload++;
          static int logged = 0;
          if (logged < 12) {
            logged++;
            D3D12_RESOURCE_DESC rd = res->GetDesc();
            TB_LOG("rt scene: upload VB view %#llx in %p base %#llx size %llu flags %#x (desc width %llu) cls %d stride %u "
                   "tris %u ps %016llx", (unsigned long long)d.vb_view, (void*)res, (unsigned long long)base,
                   (unsigned long long)size, flags, (unsigned long long)rd.Width, d.cls, d.stride, d.icount / 3,
                   (unsigned long long)d.ps);
          }
          if (g_upload_vb.size() > 65536) g_upload_vb.clear();
          g_upload_vb.insert(d.vb_view);
          delete b;
          if (pass == 0) {
            auto& v = g_dyn[(d.flags & DF_ALPHA) ? 1 : 0];
            v.push_back({d, {}});
            memcpy(v.back().x, x, sizeof x);
          }
          continue;
        } else if (d.vb + uint64_t(d.vcount - 1) * d.stride + 12 > base + size) {
          b->st = B_REJECTED;  // the view runs past the buffer
          g_st.rej_range++;
        }
        if (b->st == B_REJECTED) g_st.rejected++;
        b->dynamic = (d.flags & DF_SKINNED) != 0;
        g_blas.emplace(k, b);
      } else {
        b = it->second;
      }
      b->last_used = frame;
      if (b->st == B_REJECTED) continue;
      uint64_t ik = xk ^ (uint64_t(uintptr_t(b)) * 0x9e3779b97f4a7c15ull);
      if (!seen.insert(ik).second) {
        dup++;
        continue;
      }
      if (pass == 0) at_xform.insert(xk);
      if (b->st == B_NEW) {
        if (budget >= tris || to_build.empty()) {
          budget = budget > tris ? budget - tris : 0;
          to_build.push_back(b);
          b->st = B_BUILT;  // marked now; built below
          b->refit_frame = frame;  // a fresh build needs no refit this update
        } else {
          nblas_pending++;
          continue;
        }
      } else if (b->dynamic && b->st == B_READY && b->refit_frame != frame) {
        to_refit.push_back(b);
        b->refit_frame = frame;
      }
      D3D12_RAYTRACING_INSTANCE_DESC id{};
      memcpy(id.Transform, x, sizeof x);
      bool alpha = d.flags & DF_ALPHA;
      id.InstanceMask = pass == 0 ? (alpha ? 2 : 1) : (alpha ? 8 : 4);
      id.InstanceID = 0;  // patched below: the instance's first alpha record
      id.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
      inst.push_back(id);
      {
        InstMeta m;
        m.b = b;
        memcpy(m.x, x, sizeof x);
        m.geo0 = uint32_t(geo_src.size());
        m.ngeo = 1;
        meta.push_back(m);
        geo_src.push_back(d);
      }
      inst.back().AccelerationStructure = uint64_t(uintptr_t(b));  // patched to the address below
      (pass ? ns : ng)++;
      (pass ? tris_s : tris_g) += tris;
    }
  }

  double t1 = now_ms();
  g_st.t_draws += t1 - t0;
  g_st.views_shown = g_st.last_views;
  g_st.views_placed_shown = g_st.last_views_placed;
  g_st.last_views = g_st.last_views_placed = 0;
  // BLAS builds (one scratch region per build in this batch, one barrier after all of them).
  std::vector<std::pair<Blas*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO>> pre;
  uint64_t scratch_total = 0;
  for (Blas* b : to_build) {
    D3D12_RAYTRACING_GEOMETRY_DESC g = geo_of(*b);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = b->dynamic ? (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
                          : (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION);
    in.NumDescs = 1;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = &g;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
    pre.emplace_back(b, pi);
    scratch_total += (std::max(pi.ScratchDataSizeInBytes, pi.UpdateScratchDataSizeInBytes) + 255) & ~255ull;
  }
  for (Blas* b : to_refit) scratch_total += (b->update_scratch + 255) & ~255ull;
  bool scratch_ok = scratch_total == 0 ||
                    g_scratch.ensure(scratch_total, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  uint64_t soff = 0;
  int npost = 0;
  if (!pre.empty() && !g_post.ensure(8 * pre.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    scratch_ok = false;
  uint32_t nbuilt = 0, nrefit = 0;
  for (auto& [b, pi] : pre) {
    Alloc a = scratch_ok ? (b->dynamic ? g_final : g_staging).alloc(pi.ResultDataMaxSizeInBytes) : Alloc{};
    if (!a) {
      b->st = B_NEW;  // try again next update
      continue;
    }
    (b->dynamic ? b->as : b->staging) = a;
    b->update_scratch = pi.UpdateScratchDataSizeInBytes;
    D3D12_RAYTRACING_GEOMETRY_DESC g = geo_of(*b);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = b->dynamic ? (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
                                 : (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION);
    bd.Inputs.NumDescs = 1;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.pGeometryDescs = &g;
    bd.DestAccelerationStructureData = a.va;
    bd.ScratchAccelerationStructureData = g_scratch.va() + soff;
    soff += (std::max(pi.ScratchDataSizeInBytes, pi.UpdateScratchDataSizeInBytes) + 255) & ~255ull;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
    if (!b->dynamic) {
      pd.DestBuffer = g_post.va() + 8ull * npost;
      pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
      l->BuildRaytracingAccelerationStructure(&bd, 1, &pd);
      b->st = B_COMPACTING;
      b->post_slot = slot;
      b->post_index = npost;
      g_post_pending[slot].push_back({b, npost});
      npost++;
    } else {
      l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
      b->st = B_READY;
      b->refit_frame = frame;
    }
    nbuilt++;
    g_st.builds++;
    g_st.build_tris += b->tris;
  }
  for (Blas* b : to_refit) {
    if (!scratch_ok || !b->as) continue;
    D3D12_RAYTRACING_GEOMETRY_DESC g = geo_of(*b);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    bd.Inputs.NumDescs = 1;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.pGeometryDescs = &g;
    bd.DestAccelerationStructureData = b->as.va;
    bd.SourceAccelerationStructureData = b->as.va;
    bd.ScratchAccelerationStructureData = g_scratch.va() + soff;
    soff += (b->update_scratch + 255) & ~255ull;
    l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    nrefit++;
    g_st.refits++;
  }
  if (npost) {
    D3D12_RESOURCE_BARRIER b[2]{};
    uav_barrier(l);
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = g_post.r;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    l->ResourceBarrier(1, b);
    if (g_post_rb[slot].ensure(8ull * npost, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST))
      l->CopyBufferRegion(g_post_rb[slot].r, 0, g_post.r, 0, 8ull * npost);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    l->ResourceBarrier(1, b);
  }
  uav_barrier(l);

  // The per-update BLAS (upload-heap meshes), one instance each (identity: the transforms are
  // per geometry).
  {
    if (!(g_dyn_on.load() & 1)) g_dyn[0].clear();
    if (!(g_dyn_on.load() & 2)) g_dyn[1].clear();
    size_t total = g_dyn[0].size() + g_dyn[1].size();
    if (total && g_xf[slot].ensure(total * 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ) && g_xf[slot].cpu) {
      size_t xi = 0;
      std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geo;
      uint64_t sizes[2] = {}, scratch[2] = {};
      std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos[2];
      std::vector<DrawRec> dyn_src[2];  // geos[k]'s draws, same order (the alpha records)
      for (int k = 0; k < 2; k++) {
        for (auto& g : g_dyn[k]) {
          // Keep the build inside the buffers: the vertex count to the vertex buffer's end, and
          // the index range inside its buffer (live: the grass ring's views run past the ring).
          uint64_t vbase = 0, vsize = 0, ibase = 0, isize = 0;
          if (!buf_at(g.d.vb, nullptr, &vbase, &vsize, nullptr, nullptr) ||
              !buf_at(g.d.ib, nullptr, &ibase, &isize, nullptr, nullptr)) {
            g_st.dyn_dropped++;
            continue;
          }
          uint64_t vend = vbase + vsize;
          if (g.d.vb + 12 > vend || g.d.ib + uint64_t(g.d.icount) * (g.d.ifmt == 42 ? 4 : 2) > ibase + isize) {
            g_st.dyn_dropped++;
            continue;
          }
          uint32_t vmax = uint32_t((vend - g.d.vb - 12) / g.d.stride + 1);
          if (g.d.vcount > vmax) {
            g.d.vcount = vmax;
            g_st.dyn_clamped++;
          }
          memcpy(static_cast<uint8_t*>(g_xf[slot].cpu) + xi * 48, g.x, 48);
          D3D12_RAYTRACING_GEOMETRY_DESC gd{};
          gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
          gd.Flags = k ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
          gd.Triangles.Transform3x4 = g_xf[slot].va() + xi * 48;
          gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
          gd.Triangles.VertexCount = g.d.vcount;
          gd.Triangles.VertexBuffer.StartAddress = g.d.vb;
          gd.Triangles.VertexBuffer.StrideInBytes = g.d.stride;
          gd.Triangles.IndexFormat = DXGI_FORMAT(g.d.ifmt);
          gd.Triangles.IndexCount = g.d.icount;
          gd.Triangles.IndexBuffer = g.d.ib;
          geos[k].push_back(gd);
          dyn_src[k].push_back(g.d);
          xi++;
          (k ? g_st.last_dyn_tris_a : g_st.last_dyn_tris) += g.d.icount / 3;
        }
        if (geos[k].empty()) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs = UINT(geos[k].size());
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = geos[k].data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
        g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        sizes[k] = pi.ResultDataMaxSizeInBytes;
        scratch[k] = (pi.ScratchDataSizeInBytes + 255) & ~255ull;
      }
      bool ok = g_dyn_scratch.ensure(scratch[0] + scratch[1] + 256, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      for (int k = 0; k < 2 && ok; k++) {
        if (geos[k].empty()) continue;
        if (!g_dyn_as[k].ensure(sizes[k], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE))
          continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        bd.Inputs.NumDescs = UINT(geos[k].size());
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.pGeometryDescs = geos[k].data();
        bd.DestAccelerationStructureData = g_dyn_as[k].va();
        bd.ScratchAccelerationStructureData = g_dyn_scratch.va() + (k ? scratch[0] : 0);
        l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        D3D12_RAYTRACING_INSTANCE_DESC id{};
        id.Transform[0][0] = id.Transform[1][1] = id.Transform[2][2] = 1;
        id.InstanceMask = k ? 2 : 1;
        id.InstanceID = 0;  // patched below: the instance's first alpha record
        id.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        id.AccelerationStructure = ~0ull;  // marker: the address is final
        inst.push_back(id);
        {
          InstMeta m;
          m.geo0 = uint32_t(geo_src.size());
          m.ngeo = uint32_t(dyn_src[k].size());
          meta.push_back(m);
          geo_src.insert(geo_src.end(), dyn_src[k].begin(), dyn_src[k].end());
        }
        inst.back().AccelerationStructure = g_dyn_as[k].va() | 1ull << 63;
      }
      if (ok) uav_barrier(l);
    }
    g_st.last_dyn_geos = uint32_t(total);
    g_dyn[0].clear();
    g_dyn[1].clear();
  }

  // Instance addresses (BLAS not built or failed: dropped), then the TLAS. InstanceID becomes
  // the instance's first alpha record (CandidateInstanceID + geometry index, alpha.hlsli).
  {
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> kept;
    std::vector<InstMeta> kept_meta;
    std::vector<DrawRec> kept_geo;
    kept.reserve(inst.size());
    kept_meta.reserve(inst.size());
    kept_geo.reserve(geo_src.size());
    for (size_t i = 0; i < inst.size(); i++) {
      auto id = inst[i];
      uint64_t va;
      if (id.AccelerationStructure >> 63) {
        va = id.AccelerationStructure & ~(1ull << 63);
      } else {
        Blas* b = reinterpret_cast<Blas*>(uintptr_t(id.AccelerationStructure));
        va = b->va();
      }
      if (!va) continue;
      id.AccelerationStructure = va;
      id.InstanceID = uint32_t(kept_geo.size()) & 0xffffff;
      InstMeta m = meta[i];
      uint32_t g0 = uint32_t(kept_geo.size());
      for (uint32_t g = 0; g < m.ngeo; g++) kept_geo.push_back(geo_src[size_t(m.geo0) + g]);
      m.geo0 = g0;
      kept.push_back(id);
      kept_meta.push_back(m);
    }
    inst.swap(kept);
    meta.swap(kept_meta);
    geo_src.swap(kept_geo);
  }
  g_tlas_va = 0;
  g_tlas_instances = uint32_t(inst.size());
  if (!inst.empty() &&
      g_inst[slot].ensure(inst.size() * sizeof(inst[0]), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ) &&
      g_inst[slot].cpu) {
    memcpy(g_inst[slot].cpu, inst.data(), inst.size() * sizeof(inst[0]));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = UINT(inst.size());
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.InstanceDescs = g_inst[slot].va();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
    if (g_tlas.ensure(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) &&
        g_tlas_scratch.ensure(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
      bd.Inputs = in;
      bd.DestAccelerationStructureData = g_tlas.va();
      bd.ScratchAccelerationStructureData = g_tlas_scratch.va();
      l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
      uav_barrier(l);
      g_tlas_va = g_tlas.va();
    }
  }
  if (g_tlas_va) {
    build_alpha(l, slot, meta, geo_src);
  } else {
    g_alpha_va = 0;
    g_motion_va = 0;
    g_alpha_slot = -1;
    g_prev_x.clear();
  }
  g_st.t_build += now_ms() - t1;
  g_st.last_draws_g = dg;
  g_st.dyn_tris_shown = g_st.last_dyn_tris;
  g_st.dyn_tris_a_shown = g_st.last_dyn_tris_a;
  g_st.last_dyn_tris = g_st.last_dyn_tris_a = 0;
  g_st.last_draws_s = dsh;
  g_st.last_inst = uint32_t(inst.size());
  g_st.last_inst_g = ng;
  g_st.last_inst_s = ns;
  g_st.last_dup = dup;
  g_st.last_tris_g = tris_g;
  g_st.last_tris_s = tris_s;
  g_st.last_pending_blas = nblas_pending;
  g_st.last_builds = nbuilt;
  g_st.last_refits = nrefit;
}

// The sun / moon direction from its shadow resolve's g_mShadowProjection (3x4, view space ->
// shadow-map u, v, depth): the depth row's xyz is the light's forward axis in view space; towards
// the light is its opposite. The resolve follows the scene kick, so use the camera basis saved
// at that kick (the stereo eyes share orientation). Neither resolve shader loads ViewToWorld.
uint64_t g_sun_frames = 0;
float g_sun_world[3] = {};
void take_sun(const std::vector<DrawRec>& draws) {
  if (g_last_update == ~0ull || present_frame() - g_last_update > 1) return;
  for (auto it = draws.rbegin(); it != draws.rend(); ++it) {
    if (it->cls != DC_SUN) continue;
    float m[12];
    if (!g_cbc.read(it->cb + it->l2v, m, 48)) continue;
    double f[3] = {m[8], m[9], m[10]};
    double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 1e-12)) continue;
    double w[3];
    for (int r = 0; r < 3; r++)
      w[r] = -(g_cam_v2w[r * 4] * f[0] + g_cam_v2w[r * 4 + 1] * f[1] + g_cam_v2w[r * 4 + 2] * f[2]) / n;
    double wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(wn > 0.5 && wn < 2)) continue;
    for (int r = 0; r < 3; r++) g_sun_world[r] = float(w[r] / wn);
    trace_set_sun(g_sun_world);
    g_sun_frames++;
    return;
  }
}

bool inv4(const double* m, double* o) {  // general 4x4 inverse (row-major)
  double inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(std::fabs(det) > 1e-30)) return false;
  for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
  return true;
}

void mul4(const double* a, const double* b, double* o) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double v = 0;
      for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
      o[r * 4 + c] = v;
    }
}

// The eye's camera: view-to-world from the engine's main rend::View (+0x248, double[4][3], row
// vectors: right, up, forward, position; docs/stereo.md section 1), as the eye renders it; the
// (jittered) projection g_mViewToClip from the G-buffer draw's constants (rows 8-11, which the
// G-buffer VS reads, so they are written). Everything else derived in doubles.
bool read_camera(uint64_t cb, Camera& c, double* v2w_out, double* w2c_out) {
  void* view = stereo::main_view();
  MEMORY_BASIC_INFORMATION mi;
  if (!view || !VirtualQuery(static_cast<uint8_t*>(view) + 0x248, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
  const double* m = reinterpret_cast<const double*>(static_cast<uint8_t*>(view) + 0x248);
  double v2w[16] = {}, w2v[16], v2c[16], c2v[16], w2c[16], c2w[16];
  for (int r = 0; r < 3; r++)
    for (int k = 0; k < 4; k++) v2w[r * 4 + k] = m[k * 3 + r];
  v2w[15] = 1;
  for (int r = 0; r < 3; r++) {  // an orthonormal basis, or not a camera
    double n = v2w[r] * v2w[r] + v2w[4 + r] * v2w[4 + r] + v2w[8 + r] * v2w[8 + r];
    if (!(std::fabs(n - 1) < 1e-3)) return false;
  }
  // View-to-clip with the post-projection (TAA jitter): the View's lazy +0x2a8 (double[4][4],
  // row vectors), valid while +0x4a8 is set (it is after the engine binds the view for the
  // G-buffer). The draw constants only hold it if that draw's VS reads it (live: the prologue's
  // first G-buffer draw did not).
  const uint8_t* vb = static_cast<const uint8_t*>(view);
  if (!vb[0x4a8]) {
    g_st.cam_proj_cb++;
    float pf[16];
    if (!g_cbc.read(cb + 128, pf, 64)) return false;
    for (int i = 0; i < 16; i++) v2c[i] = pf[i];
  } else {
    const double* pm = reinterpret_cast<const double*>(vb + 0x2a8);
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++) v2c[r * 4 + k] = pm[k * 4 + r];
  }
  if (!inv4(v2w, w2v) || !inv4(v2c, c2v)) return false;
  mul4(v2c, w2v, w2c);
  if (!inv4(w2c, c2w)) return false;
  auto f = [](const double* d, float* o) {
    for (int i = 0; i < 16; i++) o[i] = float(d[i]);
  };
  f(w2v, c.w2v);
  f(v2w, c.v2w);
  f(v2c, c.v2c);
  f(c2v, c.c2v);
  f(w2c, c.w2c);
  f(c2w, c.c2w);
  for (int i = 0; i < 12; i++) v2w_out[i] = v2w[i];
  if (w2c_out) memcpy(w2c_out, w2c, sizeof w2c);  // absolute world -> clip, for history reprojection
  if (g_dump.load() > 0) {
    auto m = [](const char* n, const float* f) {
      TB_LOG("rt dump cam %s: %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f", n, f[0],
             f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    };
    m("w2v", c.w2v);
    m("v2w", c.v2w);
    m("v2c", c.v2c);
    m("c2v", c.c2v);
  }
  return true;
}

// The kick: everything for this eye, on our list, submitted right after the engine's lists.
void run_frame(ID3D12CommandQueue* q) {
  if (!gpu_init(q)) return;
  if (q != g_q) return;  // only the game's main queue
  LARGE_INTEGER t0, t1, fq;
  QueryPerformanceCounter(&t0);
  run_deferred();
  uint64_t frame = present_frame();
  int eye = stereo::eye_now() > 0 ? 1 : 0;
  std::vector<DrawRec> draws;
  {
    std::lock_guard lk(g_pending_mu);
    draws.swap(g_pending);
  }
  take_sun(draws);
  int slot = int(g_submits % kSlots);
  if (g_slot_fv[slot] > done_fv()) {  // GPU more than kSlots submissions behind: skip, never wait
    g_st.skipped_busy++;
    return;
  }
  trace_slot_done(slot);
  // The eye's camera and G-buffer targets: its first G-buffer draw.
  EyeInputs in;
  const DrawRec* first = nullptr;
  for (auto& d : draws)
    if (d.cls == DC_GBUFFER && d.rt0 && d.rt1) {
      first = &d;
      break;
    }
  double v2w[12];
  if (!first || !read_camera(first->cb, in.cam, v2w, in.w2c)) {
    g_st.skipped_nocam++;
    return;
  }
  in.depth = first->rt0;
  in.normal = first->rt1;
  bool build = frame != g_last_update;
  if (build) {
    // World origin of this update's TLAS: the camera position (row translations of ViewToWorld).
    g_anchor[0] = v2w[3];
    g_anchor[1] = v2w[7];
    g_anchor[2] = v2w[11];
    memcpy(g_cam_v2w, v2w, sizeof v2w);
  } else {
    // Second eye of the same update: its draws are the same scene; keep only the shadow-map
    // draws not yet consumed for the next update.
    std::vector<DrawRec> keep;
    for (auto& d : draws)
      if (d.cls == DC_SHADOW) keep.push_back(d);
    draws.swap(keep);
  }
  g_alloc[slot]->Reset();
  g_list->Reset(g_alloc[slot], nullptr);
  trace_mark(g_list, slot, 0);
  uav_barrier(g_list);  // our builds read what the engine's lists wrote (skinned vertices)
  if (build) {
    process_invalidations();
    evict(frame);
    build_scene(g_list, draws, slot, frame);
    g_last_update = frame;
  } else if (!draws.empty()) {
    std::lock_guard lk(g_pending_mu);
    g_pending.insert(g_pending.end(), draws.begin(), draws.end());
  }
  in.eye = eye;
  memcpy(in.anchor, g_anchor, sizeof g_anchor);
  in.depth_state = gpu_state(in.depth);
  in.normal_state = gpu_state(in.normal);
  in.tlas = g_tlas_va;
  in.frame = frame;
  in.alpha = g_alpha_va;
  in.alpha_slot = g_alpha_slot;
  in.motion = g_motion_va;
  trace_record(g_list, in, slot);
  if (FAILED(g_list->Close())) {
    TB_LOG("rt scene: our command list failed to close; ray-traced passes off");
    g_on = false;
    scene_capture(false);
    return;
  }
  ID3D12CommandList* ls[] = {g_list};
  exec_lists(q, 1, ls);
  q->Signal(g_fence, ++g_fv);
  g_slot_fv[slot] = g_fv;
  g_submits++;
  g_st.frames++;
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&fq);
  g_st.cpu_ms_sum += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart);
  g_st.cpu_n++;
}

GrowBuf g_spot_cb[kSlots];
uint64_t g_spot_frame = ~0ull;
uint64_t g_spot_cb_va = 0;
std::atomic<float> g_spot_bias{0.03f}, g_spot_radius{0.04f}, g_spot_rays{4}, g_spot_clear{0.35f};
std::atomic<float> g_spot_maxdist{3.0f}, g_spot_mincos{0.5f};
std::atomic<int> g_spot_all{0};

}  // namespace

bool scene_spot_ready() { return g_on.load(std::memory_order_relaxed) && g_tlas_va && g_last_update == present_frame(); }

// Game thread, at a swapped spotlight draw (after the kick: the TLAS of this update is built).
bool scene_spot_bind(uint64_t* tlas, uint64_t* cb, uint64_t* alpha) {
  if (!scene_spot_ready()) return false;
  uint64_t f = present_frame();
  if (f != g_spot_frame) {
    int slot = int((g_submits + kSlots - 1) % kSlots);  // the slot of this update's submission
    if (!g_spot_cb[slot].ensure(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !g_spot_cb[slot].cpu)
      return false;
    float c[12] = {float(g_anchor[0]), float(g_anchor[1]), float(g_anchor[2]), float(0x0f), g_spot_bias, g_spot_radius,
                   g_spot_rays, g_spot_clear, g_alpha_va ? 1.0f : 0.0f, g_spot_maxdist, g_spot_mincos,
                   float(g_spot_all.load())};
    memcpy(g_spot_cb[slot].cpu, c, sizeof c);
    g_spot_cb_va = g_spot_cb[slot].va();
    g_spot_frame = f;
  }
  *tlas = g_tlas_va;
  *cb = g_spot_cb_va;
  *alpha = g_alpha_va ? g_alpha_va : g_tlas_va;  // unread while tb_q.x is 0, but never null
  return true;
}

void scene_spot_config(const std::string& id, const std::string& v) {
  double x = atof(v.c_str());
  if (id == "dev.rt_spot_bias" && !v.empty()) g_spot_bias = float(x);
  else if (id == "dev.rt_spot_radius" && !v.empty()) g_spot_radius = float(x);
  else if (id == "dev.rt_spot_rays" && !v.empty()) g_spot_rays = float(x);
  else if (id == "dev.rt_spot_clear" && !v.empty()) g_spot_clear = float(x);
  else if (id == "dev.rt_spot_maxdist" && !v.empty()) g_spot_maxdist = float(x);
  else if (id == "dev.rt_spot_mincos" && !v.empty()) g_spot_mincos = float(x);
  else if (id == "dev.rt_spot_all" && !v.empty()) g_spot_all = x != 0 ? 1 : 0;
  g_spot_frame = ~0ull;  // rewrite the constants at the next swapped draw
}

// ---- census.cpp / rt.cpp callbacks --------------------------------------------------------------

void scene_after_exec(ID3D12CommandQueue* q, unsigned n, ID3D12CommandList* const*, std::vector<DrawRec>& draws) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  if (!draws.empty()) {
    std::lock_guard lk(g_pending_mu);
    if (g_pending.size() < 200000) g_pending.insert(g_pending.end(), draws.begin(), draws.end());
  }
  // The kick: the first multi-list submission inside the main pass, before HBAO (live census:
  // exactly one, n = 14..15, on the game thread).
  if (t_main == 1 && n > 1) {
    t_main = 2;
    run_frame(q);
  }
}

void scene_invalidate(uint64_t va, uint64_t size) {
  std::lock_guard lk(g_inv_mu);  // never take another lock under this one (census holds g_mu)
  if (g_inv.size() < 100000) g_inv.emplace_back(va, size);
}

std::atomic<uint64_t> g_ao_copies{0}, g_ao_copy_miss{0}, g_sun_copies{0}, g_sun_copy_miss{0};

// Copy `src` over `dst` in the engine's list being recorded on this thread (sizes and format
// checked), transitioning dst from its record-time state and back.
bool copy_into_engine(ID3D12Resource* src, ID3D12Resource* dst, DXGI_FORMAT want) {
  ID3D12GraphicsCommandList* l = cur_list();
  if (!src || !dst || !l) return false;
  D3D12_RESOURCE_DESC a = src->GetDesc(), b = dst->GetDesc();
  if (a.Width != b.Width || a.Height != b.Height || b.Format != want || b.SampleDesc.Count != 1 || b.MipLevels != 1 ||
      b.DepthOrArraySize != 1)
    return false;
  unsigned st = res_state(dst);
  D3D12_RESOURCE_BARRIER br{};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = dst;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = D3D12_RESOURCE_STATES(st);
  br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  if (st != D3D12_RESOURCE_STATE_COPY_DEST) l->ResourceBarrier(1, &br);
  l->CopyResource(dst, src);
  std::swap(br.Transition.StateBefore, br.Transition.StateAfter);
  if (st != D3D12_RESOURCE_STATE_COPY_DEST) l->ResourceBarrier(1, &br);
  return true;
}

void scene_mark(int pass, int phase, void* obj) {
  // rt.cpp pass ids: 0 main pass, 3 HBAO render, 4 HBAO apply.
  if (pass == 0) t_main = phase == 0 ? 1 : 0;
  else if (pass == 3 && phase == 0 && t_main == 1) t_main = 2;  // no kick before HBAO: none this frame
  if (pass == 5 && phase == 1 && g_on.load(std::memory_order_relaxed)) {
    // End of the shadow-buffer pass (0x52d5c0: the sun / moon cascades resolved into pShadowRT,
    // R = shadow): the ray-traced visibility replaces it (prototype, [dev] rt_sun = 1). G and B
    // are not read by the lighting (static: the consumers read .r and .a); A is written next by
    // the AO apply.
    int eye = stereo::eye_now() > 0 ? 1 : 0;
    ID3D12Resource* src = trace_sun_result(eye, present_frame());
    if (src) (copy_into_engine(src, pool_resource("pShadowRT"), DXGI_FORMAT_B8G8R8A8_UNORM) ? g_sun_copies
                                                                                               : g_sun_copy_miss)++;
    return;
  }
  if (pass != 4 || phase != 0 || !obj || !g_on.load(std::memory_order_relaxed)) return;
  // HBAO apply (0x537ef0) copies g_sHBAOBuffer.r (object +0x18, a pool RT, R16G16_FLOAT) into
  // pShadowRT.a (its PS 5e551338353d5d50 samples .r and writes it to all four channels under
  // write mask 0x8; static, disassembly). Put this eye's RTAO result into that buffer first, in
  // the engine's own list (the apply is recorded right after this).
  int eye = stereo::eye_now() > 0 ? 1 : 0;
  ID3D12Resource* src = trace_ao_result(eye, present_frame());
  ID3D12Resource* dst = engine_resource(*reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + 0x18));
  if (!src) return;
  if (!copy_into_engine(src, dst, DXGI_FORMAT_R16G16_FLOAT)) {
    g_ao_copy_miss++;
    return;
  }
  g_ao_copies++;
}

bool scene_enable(bool on) {
  if (on == g_on.load()) return true;
  if (on && !scene_capture(true)) return false;
  if (!on) scene_capture(false);
  g_on = on;
  if (!on) {
    std::lock_guard lk(g_pending_mu);
    g_pending.clear();
  }
  TB_LOG("rt scene: %s", on ? "on" : "off");
  return true;
}

bool scene_enabled() { return g_on; }

std::string scene_status() {
  uint64_t blas_n = g_blas.size(), ready = 0, dyn = 0, tris = 0;
  for (auto& [k, b] : g_blas) {
    if (b->va()) ready++;
    if (b->dynamic) dyn++;
    tris += b->tris;
  }
  char buf[8192];
  snprintf(buf, sizeof buf,
           "{\"on\":%s,\"capture\":%s,\"frames\":%llu,\"skipped_busy\":%llu,\"skipped_nocam\":%llu,\"cam_proj_from_cb\":%llu,"
           "\"blas\":{\"entries\":%llu,\"with_as\":%llu,\"dynamic\":%llu,\"tris\":%llu,\"rejected\":%llu,\"rejected_unknown\":%llu,\"rejected_upload\":%llu,\"rejected_range\":%llu,"
           "\"builds\":%llu,\"build_tris\":%llu,\"refits\":%llu,\"compactions\":%llu,\"evictions\":%llu,"
           "\"invalidated\":%llu,\"final_used_mb\":%.2f,\"final_reserved_mb\":%.2f,\"staging_used_mb\":%.2f,"
           "\"staging_reserved_mb\":%.2f,\"scratch_mb\":%.2f},"
           "\"last\":{\"draws_gbuffer\":%u,\"draws_shadow\":%u,\"instances\":%u,\"inst_gbuffer\":%u,\"inst_shadow_only\":%u,"
           "\"duplicates\":%u,\"tris_gbuffer\":%llu,\"tris_shadow_only\":%llu,\"blas_waiting\":%u,\"builds\":%u,"
           "\"refits\":%u,\"tlas_mb\":%.2f,\"per_update_geometries\":%u,\"per_update_tris\":%llu,"
           "\"per_update_tris_alpha\":%llu,\"per_update_mb\":%.2f,\"per_update_dropped\":%llu,\"per_update_clamped\":%llu,\"shadow_views\":%u,\"shadow_views_placed\":%u},\"cpu_ms_mean\":%.3f,\"ao_copies\":%llu,\"ao_copy_miss\":%llu,"
           "\"cpu_ms\":{\"instances\":%.3f,\"builds_tlas\":%.3f},\"skip_upload\":%llu,\"sun\":{\"frames\":%llu,\"dir\":[%.4f,%.4f,%.4f],\"copies\":%llu,\"copy_miss\":%llu},"
           "\"alpha\":{\"records\":%u,\"jobs\":%u,\"tex\":%u,\"bufs\":%u,\"fallback\":%u,\"uv_mb\":%.2f},"
           "\"capture\":%s,\"trace\":%s}",
           g_on ? "true" : "false", scene_capture_on() ? "true" : "false", (unsigned long long)g_st.frames,
           (unsigned long long)g_st.skipped_busy, (unsigned long long)g_st.skipped_nocam, (unsigned long long)g_st.cam_proj_cb, (unsigned long long)blas_n,
           (unsigned long long)ready, (unsigned long long)dyn, (unsigned long long)tris, (unsigned long long)g_st.rejected, (unsigned long long)g_st.rej_unknown,
           (unsigned long long)g_st.rej_upload, (unsigned long long)g_st.rej_range,
           (unsigned long long)g_st.builds, (unsigned long long)g_st.build_tris, (unsigned long long)g_st.refits,
           (unsigned long long)g_st.compactions, (unsigned long long)g_st.evictions, (unsigned long long)g_st.invalidated,
           g_final.used / 1048576.0, g_final.reserved / 1048576.0, g_staging.used / 1048576.0,
           g_staging.reserved / 1048576.0, g_scratch.size / 1048576.0, g_st.last_draws_g, g_st.last_draws_s,
           g_st.last_inst, g_st.last_inst_g, g_st.last_inst_s, g_st.last_dup, (unsigned long long)g_st.last_tris_g,
           (unsigned long long)g_st.last_tris_s, g_st.last_pending_blas, g_st.last_builds, g_st.last_refits,
           g_tlas.size / 1048576.0, g_st.last_dyn_geos, (unsigned long long)g_st.dyn_tris_shown,
           (unsigned long long)g_st.dyn_tris_a_shown, (g_dyn_as[0].size + g_dyn_as[1].size) / 1048576.0,
           (unsigned long long)g_st.dyn_dropped, (unsigned long long)g_st.dyn_clamped, g_st.views_shown, g_st.views_placed_shown, g_st.cpu_n ? g_st.cpu_ms_sum / double(g_st.cpu_n) : 0.0,
           (unsigned long long)g_ao_copies.load(), (unsigned long long)g_ao_copy_miss.load(),
           g_st.cpu_n ? g_st.t_draws / double(g_st.cpu_n) : 0.0, g_st.cpu_n ? g_st.t_build / double(g_st.cpu_n) : 0.0,
           (unsigned long long)g_st.skip_upload, (unsigned long long)g_sun_frames, g_sun_world[0], g_sun_world[1], g_sun_world[2],
           (unsigned long long)g_sun_copies.load(), (unsigned long long)g_sun_copy_miss.load(),
           g_ast.records, g_ast.jobs, g_ast.tex, g_ast.bufs, g_ast.fallback, g_ast.uv_bytes / 1048576.0,
           scene_capture_status().c_str(), trace_status().c_str());
  return buf;
}

std::string scene_verb(const std::string& sub, const std::string& arg) {
  if (sub == "budget") g_build_budget_tris = atoi(arg.c_str());
  else if (sub == "buf") {
    ID3D12Resource* r = nullptr;
    uint64_t base = 0, size = 0;
    int heap = -1;
    unsigned flags = 0;
    bool ok = buf_at(strtoull(arg.c_str(), nullptr, 0), &r, &base, &size, &heap, &flags);
    char b[200];
    snprintf(b, sizeof b, "{\"found\":%s,\"res\":\"%p\",\"base\":%llu,\"size\":%llu,\"heap\":%d,\"flags\":%u,\"upload_skip\":%d}",
             ok ? "true" : "false", (void*)r, (unsigned long long)base, (unsigned long long)size, heap, flags,
             int(g_upload_vb.count(strtoull(arg.c_str(), nullptr, 0))));
    return b;
  }
  else if (sub == "dump") {
    g_dump_ps = arg.find(':') != std::string::npos ? strtoull(arg.c_str() + arg.find(':') + 1, nullptr, 16) : 0;
    g_dump = arg.empty() ? 20 : atoi(arg.c_str());
  }
  else if (sub == "casters") {
    if (arg == "0") g_shadow_casters = CasterGrouping::Off;
    else if (arg == "atlas") g_shadow_casters = CasterGrouping::AtlasTile;
    else g_shadow_casters = CasterGrouping::Projection;
  }
  else if (sub == "dyn") g_dyn_on = atoi(arg.c_str());
  else if (sub == "reset") {
    g_st = Stats{};
    trace_timing_reset();
  }
  return scene_status();
}

}  // namespace tb::rt
