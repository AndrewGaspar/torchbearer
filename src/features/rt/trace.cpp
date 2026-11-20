// Ray-traced passes on the scene's TLAS (rt_scene.h, docs/raytracing.md "RTAO"):
//  - rg_val: primary rays from the camera, hit distance against the G-buffer depth (validation:
//    a histogram of the relative error, optionally a colour-coded image);
//  - rg_ao: short ambient-occlusion rays (cosine-weighted around the G-buffer normal, first hit
//    ends), 1..N per pixel;
//  - cs_temporal: accumulation with this eye's own history, reprojected by the camera (the
//    engine's motion vectors are written after the lighting, so not yet available at this point
//    of the frame), rejected on a linear-depth mismatch;
//  - cs_atrous x2: depth/normal-aware spatial filter; the last one writes the result in the
//    format of the engine's HBAO buffer (R16G16_FLOAT, AO in R), which rt.cpp copies into
//    g_sHBAOBuffer right before the engine's HBAO apply puts it into pShadowRT.a.
// Shaders are HLSL compiled at run time with the game's own dxcompiler.dll (1.4.1907: lib_6_3 and
// cs_6_0; DXR 1.0, no inline ray queries), so the DLL embeds no third-party shader code.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/lab.h"
#include "core/log.h"
#include "features/rt/dxc.h"
#include "features/rt/rt_internal.h"
#include "features/rt/rt_scene.h"

namespace tb::rt {
namespace {

constexpr int kSlots = 4;
constexpr int kSets = 16;     // descriptor sets per slot
constexpr int kPerSet = 12;   // t1..t8, u0..u3
constexpr int kTs = 16;       // timestamps per slot
constexpr int kCbStride = 512;  // constant-buffer entries (sizeof(Cb), 256-aligned)

const char* kCommon = R"(
cbuffer C : register(b0) {
  row_major float4x4 g_c2v;   // clip -> view (this eye, jittered, as the engine's $Globals)
  row_major float4x4 g_v2w;   // view -> world relative to the TLAS anchor
  row_major float4x4 g_v2pc;  // view -> this eye's previous clip
  float4 g_p0;  // AO radius (m), normal bias (m), frame index, history length
  float4 g_p1;  // width, height, 1/width, 1/height
  uint4 g_u0;   // instance mask, samples per pixel | mode, a-trous step, history valid
  float4 g_sun; // world direction to the light, cone half-angle (radians)
  float4 g_p2;  // AO strength (exponent), validation ray length, alpha-tested coverage, -
};
Texture2D<float> t_depth : register(t1);
Texture2D<float4> t_normal : register(t2);
Texture2D<float2> t_a : register(t3);
Texture2D<float2> t_b : register(t4);
Texture2D<float> t_c : register(t5);
RWTexture2D<float2> u_a : register(u0);
RWTexture2D<float> u_b : register(u1);
RWByteAddressBuffer u_stats : register(u2);
RWTexture2D<float4> u_vis : register(u3);
SamplerState s_lin : register(s0);
SamplerState s_pt : register(s1);

bool is_geo(float z) { return z > 0.0 && z < 1.0; }
float3 view_pos(int2 px, float z) {
  float2 ndc = (float2(px) + 0.5) * g_p1.zw * float2(2, -2) + float2(-1, 1);
  float4 v = mul(g_c2v, float4(ndc, z, 1));
  return v.xyz / v.w;
}
int2 clampi(int2 q) { return clamp(q, int2(0, 0), int2(g_p1.xy) - 1); }
float3 view_at(int2 q) { q = clampi(q); return view_pos(q, t_depth[q]); }
// Geometric normal from the depth buffer (the side with the smaller depth step), facing the eye.
float3 geo_normal(int2 px, float3 p) {
  float3 r = view_at(px + int2(1, 0)), l = view_at(px - int2(1, 0));
  float3 d = view_at(px + int2(0, 1)), u = view_at(px - int2(0, 1));
  float3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
  float3 dy = abs(d.z - p.z) < abs(p.z - u.z) ? d - p : p - u;
  float3 n = normalize(cross(dy, dx));
  return dot(n, -p) < 0 ? -n : n;
}
float3 shading_normal(int2 px, float3 ng) {
  float3 n = t_normal[px].xyz * 2 - 1;
  float l = length(n);
  n = l > 0.1 ? n / l : ng;
  return dot(n, ng) < 0.1 ? ng : n;
}
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float2 rnd2(int2 px, uint i) {
  uint h = hash(uint(px.x) * 1973u + uint(px.y) * 9277u + 26699u);
  float2 b = float2(h & 0xffff, h >> 16) / 65536.0;
  return frac(b + float2(0.7548776662, 0.5698402910) * float(i % 4096u));
}
float3 cos_dir(float3 n, float2 u) {
  float s = n.z >= 0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float b = n.x * n.y * a;
  float3 t = float3(1 + s * n.x * n.x * a, s * b, -s * n.x);
  float3 bt = float3(b, s + n.y * n.y * a, -n.y);
  float r = sqrt(u.x), ph = 6.2831853 * u.y;
  return normalize(t * (r * cos(ph)) + bt * (r * sin(ph)) + n * sqrt(max(0.0, 1.0 - u.x)));
}
)";

const char* kLib = R"(
RaytracingAccelerationStructure Scene : register(t0);
struct Pay { float t; };
[shader("miss")] void ms(inout Pay p) { p.t = -1; }
[shader("closesthit")] void ch(inout Pay p, in BuiltInTriangleIntersectionAttributes a) { p.t = RayTCurrent(); }

[shader("raygeneration")] void rg_ao() {
  int2 px = int2(DispatchRaysIndex().xy);
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1e4); return; }
  float3 p = view_pos(px, z);
  float3 ng = geo_normal(px, p);
  float3 ns = shading_normal(px, ng);
  float3 wp = mul(g_v2w, float4(p, 1)).xyz;
  float3 wn = normalize(mul((float3x3)g_v2w, ns));
  float3 wg = normalize(mul((float3x3)g_v2w, ng));
  float3 o = wp + wg * (g_p0.y + length(p) * 0.0015);
  uint spp = max(g_u0.y & 0xff, 1u);
  float vis = 0;
  for (uint s = 0; s < spp; s++) {
    float3 dir = cos_dir(wn, rnd2(px, uint(g_p0.z) * spp + s));
    float dg = dot(dir, wg);
    if (dg < 0.02) dir = normalize(dir - wg * (dg - 0.02) * 2.0);
    RayDesc r;
    r.Origin = o;
    r.Direction = dir;
    r.TMin = 0.01 + length(p) * 0.002;  // past the receiver's own surface (skinned / LOD mismatch)
    r.TMax = g_p0.x;
    Pay pl;
    pl.t = 0;
    // Opaque instances first; alpha-tested ones (no any-hit alpha test yet) occlude with a
    // probability, their average coverage (g_p2.z).
    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
             g_u0.x & 0x5, 0, 0, 0, r, pl);
    bool occ = pl.t >= 0;
    if (!occ && (g_u0.x & 0xa) && frac(rnd2(px, uint(g_p0.z) * spp + s + 977u).x) < g_p2.z) {
      pl.t = 0;
      TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
               g_u0.x & 0xa, 0, 0, 0, r, pl);
      occ = pl.t >= 0;
    }
    vis += occ ? 0.0 : 1.0;
  }
  u_a[px] = float2(vis / float(spp), p.z);
}

// Sun / moon visibility (phase 3 prototype): rays towards the light inside its cone.
[shader("raygeneration")] void rg_sun() {
  int2 px = int2(DispatchRaysIndex().xy);
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1e4); return; }
  float3 p = view_pos(px, z);
  float3 ng = geo_normal(px, p);
  float3 wp = mul(g_v2w, float4(p, 1)).xyz;
  float3 wg = normalize(mul((float3x3)g_v2w, ng));
  float3 L = normalize(g_sun.xyz);
  float vis = 0;
  uint spp = max(g_u0.y & 0xff, 1u);
  for (uint s = 0; s < spp; s++) {
    float2 u = rnd2(px, uint(g_p0.z) * spp + s);
    // uniform direction in the cone around L
    float ct = 1.0 - u.x * (1.0 - cos(g_sun.w)), st = sqrt(max(0.0, 1.0 - ct * ct)), ph = 6.2831853 * u.y;
    float3 t = normalize(abs(L.y) < 0.99 ? cross(L, float3(0, 1, 0)) : cross(L, float3(1, 0, 0)));
    float3 b = cross(L, t);
    float3 dir = normalize(L * ct + t * (st * cos(ph)) + b * (st * sin(ph)));
    if (dot(dir, wg) <= 0) continue;
    RayDesc r;
    r.Origin = wp + wg * (g_p0.y + length(p) * 0.0015);
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = 1000.0;
    Pay pl;
    pl.t = 0;
    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
             g_u0.x & 0xff, 0, 0, 0, r, pl);
    vis += pl.t < 0 ? 1.0 : 0.0;
  }
  u_a[px] = float2(vis / float(spp), p.z);
}

// Validation: a primary ray per pixel, its hit distance against the G-buffer's.
// Bins (x2: pixels whose normal-buffer alpha is 1, the foliage material term, in bins 8..15):
// 0 sky + miss, 1 sky + hit, 2 geometry + miss, 3 |err| < 0.2 %, 4 < 1 %, 5 < 5 %,
// 6 ray nearer by > 5 %, 7 ray farther by > 5 %.
[shader("raygeneration")] void rg_val() {
  int2 px = int2(DispatchRaysIndex().xy);
  float z = t_depth[px];
  bool sky = !is_geo(z);
  float3 p = view_pos(px, sky ? 0.99999 : z);
  float3 cam = float3(g_v2w[0].w, g_v2w[1].w, g_v2w[2].w);
  RayDesc r;
  r.Origin = cam;
  r.Direction = normalize(mul((float3x3)g_v2w, p));
  r.TMin = 0.0;
  r.TMax = g_p2.y;
  Pay pl;
  pl.t = 0;
  TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, g_u0.x & 0xff, 0, 0, 0, r, pl);
  float d = length(p);
  uint bin;
  float4 c;
  if (sky) {
    bin = pl.t < 0 ? 0 : 1;
    c = pl.t < 0 ? float4(0, 0, 0, 1) : float4(0.5, 0, 0.5, 1);
  } else if (pl.t < 0) {
    bin = 2;
    c = float4(1, 0, 1, 1);
  } else {
    float e = (pl.t - d) / d, ae = abs(e);
    if (ae < 0.002) { bin = 3; c = float4(0, 0.8, 0, 1); }
    else if (ae < 0.01) { bin = 4; c = float4(0.5, 1, 0.5, 1); }
    else if (ae < 0.05) { bin = 5; c = float4(1, 1, 0, 1); }
    else if (e < 0) { bin = 6; c = float4(1, 0, 0, 1); }
    else { bin = 7; c = float4(0, 0.3, 1, 1); }
  }
  if (!sky && t_normal[px].a > 0.99) bin += 8;
  uint dummy;
  u_stats.InterlockedAdd(bin * 4, 1, dummy);
  if (g_u0.z) u_vis[px] = c;
}
)";

const char* kCs = R"(
[numthreads(8, 8, 1)] void cs_temporal(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float2 raw = t_a[px];
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1); u_b[px] = 1e4; return; }
  float3 p = view_pos(px, z);
  float n = 1, ao = raw.x;
  if (g_u0.w != 0) {
    float4 pc = mul(g_v2pc, float4(p, 1));
    if (pc.w > 0.01) {
      float2 uv = pc.xy / pc.w * float2(0.5, -0.5) + 0.5;
      if (all(uv > 0) && all(uv < 1)) {
        float pz = t_c.SampleLevel(s_pt, uv, 0);
        if (abs(pz - pc.w) < 0.04 * pc.w + 0.03) {
          float2 h = t_b.SampleLevel(s_lin, uv, 0);
          n = min(h.y + 1, g_p0.w);
          ao = lerp(h.x, raw.x, 1.0 / n);
        }
      }
    }
  }
  u_a[px] = float2(ao, n);
  u_b[px] = p.z;
}

static const float kW[3] = {0.375, 0.25, 0.0625};
[numthreads(8, 8, 1)] void cs_atrous(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float2 c = t_a[px];
  uint stage = g_u0.y >> 8;  // 1 intermediate, 2 final AO (u0, R16G16), 3 final sun (u3, BGRA8)
  if (!is_geo(t_depth[px])) {
    if (stage == 3) u_vis[px] = float4(1, 1, 1, 1);
    else u_a[px] = stage == 2 ? float2(1, 1) : c;
    return;
  }
  float zc = t_c[px];
  float3 nc = t_normal[px].xyz * 2 - 1;
  int step = int(g_u0.z);
  // fewer samples accumulated: wider support
  float sigz = 0.02 * zc + 0.02;
  float sum = 0, ws = 0;
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int2 q = clampi(px + int2(dx, dy) * step);
      float zq = t_c[q];
      float3 nq = t_normal[q].xyz * 2 - 1;
      float w = kW[abs(dx)] * kW[abs(dy)] * exp(-abs(zq - zc) / sigz) * pow(saturate(dot(nc, nq)), 8.0);
      sum += t_a[q].x * w;
      ws += w;
    }
  float ao = ws > 1e-5 ? sum / ws : c.x;
  if (stage == 3) {
    u_vis[px] = float4(saturate(ao), 1, 1, 1);
  } else if (stage == 2) {
    ao = pow(saturate(ao), g_p2.x);
    u_a[px] = float2(ao, ao);
  } else {
    u_a[px] = float2(ao, c.y);
  }
}
)";

struct Cb {
  float c2v[16], v2w[16], v2pc[16];
  float p0[4], p1[4];
  uint32_t u0[4];
  float sun[4], p2[4];
};

static_assert(sizeof(Cb) <= kCbStride, "constant entries overlap");

template <typename T>
void rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

ID3D12Device5* g_dev = nullptr;
ID3D12RootSignature* g_rs = nullptr;
ID3D12StateObject* g_so = nullptr;
ID3D12Resource* g_table = nullptr;  // rg_ao, rg_val, rg_sun, ms, hg: 64 bytes each
ID3D12PipelineState* g_cs_temporal = nullptr;
ID3D12PipelineState* g_cs_atrous = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
ID3D12Resource* g_cb[kSlots] = {};
uint8_t* g_cb_cpu[kSlots] = {};
ID3D12QueryHeap* g_qh = nullptr;
ID3D12Resource* g_ts_rb = nullptr;
UINT64 g_ts_freq = 0;
int g_ts_n[kSlots] = {};

struct Tex {
  ID3D12Resource* r = nullptr;
  D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
uint32_t g_w = 0, g_h = 0;
Tex g_raw, g_tmp, g_vis;
// Per kind (0 AO, 1 sun / moon visibility) and eye: history ping-pong, linear depth ping-pong,
// the final result (AO: R16G16_FLOAT like g_sHBAOBuffer; sun: B8G8R8A8 like pShadowRT).
struct Chain {
  Tex hist[2], lin[2], out;
  int cur = 0;
  uint64_t frame = ~0ull;      // last traced update
  float prev_w2c[16] = {};
};
Chain g_ch[2][2];
bool g_bgra_uav = false;  // B8G8R8A8_UNORM typed UAV stores (the sun result)
ID3D12Resource* g_stats = nullptr;
ID3D12Resource* g_stats_rb[kSlots] = {};
ID3D12Resource* g_vis_rb = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_vis_fp{};
bool g_slot_val[kSlots] = {}, g_slot_img[kSlots] = {};

std::mutex g_mu;  // status / arming vs the game thread
struct Val {
  bool armed = false, image = false;
  unsigned mask = 0x0f;
  bool have = false;
  uint32_t bins[16] = {};
  std::string image_path;
  uint64_t frame = 0;
} g_val;
struct Timing {
  double sum[kTs] = {};
  uint64_t n[kTs] = {};
} g_tm;
uint64_t g_traced = 0, g_skipped_state = 0;

// Config (graphics.cpp sets [graphics] ao; the [dev] rtao_* keys tune it).
std::atomic<float> g_radius{1.5f}, g_strength{1.0f}, g_bias{0.02f}, g_hist_len{24.f};
std::atomic<int> g_spp{1}, g_mask{0x0f}, g_mode{0};
std::atomic<float> g_alpha_cov{0.4f};  // mode: 0 AO, 1 raw AO, 2 sun visibility
std::atomic<float> g_sun_dir[3] = {0.3f, 0.8f, 0.2f};
std::atomic<float> g_sun_cone{0.0047f};
std::atomic<bool> g_ao_on{true};
std::atomic<bool> g_sun_on{false}, g_sun_override{false};
std::atomic<int> g_sun_spp{1}, g_sun_mask{0x0f};

using SerializeRS_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                       ID3DBlob**);

ID3D12Resource* buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = d.DepthOrArraySize = d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = fl;
  ID3D12Resource* r = nullptr;
  g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource), (void**)&r);
  return r;
}

bool tex(Tex& t, DXGI_FORMAT f, uint32_t w, uint32_t h) {
  rel(t.r);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = d.MipLevels = 1;
  d.Format = f;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  t.st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  t.fmt = f;
  return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.st, nullptr,
                                                  __uuidof(ID3D12Resource), (void**)&t.r));
}

void to(ID3D12GraphicsCommandList4* l, Tex& t, D3D12_RESOURCE_STATES s) {
  if (!t.r || t.st == s) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = t.r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = t.st;
  b.Transition.StateAfter = s;
  l->ResourceBarrier(1, &b);
  t.st = s;
}
void uav(ID3D12GraphicsCommandList4* l) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  l->ResourceBarrier(1, &b);
}

bool chain_alloc(Chain& c, int kind) {
  if (c.out.r) return true;
  bool ok = tex(c.out, kind ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R16G16_FLOAT, g_w, g_h);
  for (int i = 0; i < 2 && ok; i++)
    ok = tex(c.hist[i], DXGI_FORMAT_R16G16_FLOAT, g_w, g_h) && tex(c.lin[i], DXGI_FORMAT_R32_FLOAT, g_w, g_h);
  c.frame = ~0ull;
  if (!ok) {
    TB_LOG("rt trace: could not allocate the %s chain", kind ? "sun" : "AO");
    for (int i = 0; i < 2; i++) {
      rel(c.hist[i].r);
      rel(c.lin[i].r);
    }
    rel(c.out.r);
  }
  return ok;
}

bool targets(uint32_t w, uint32_t h) {
  if (w == g_w && h == g_h && g_raw.r) return true;
  for (auto& k : g_ch)
    for (auto& c : k) {
      for (int i = 0; i < 2; i++) {
        rel(c.hist[i].r);
        rel(c.lin[i].r);
      }
      rel(c.out.r);
      c.frame = ~0ull;
    }
  bool ok = tex(g_raw, DXGI_FORMAT_R16G16_FLOAT, w, h) && tex(g_tmp, DXGI_FORMAT_R16G16_FLOAT, w, h) &&
            tex(g_vis, DXGI_FORMAT_R8G8B8A8_UNORM, w, h);
  if (!ok) {
    TB_LOG("rt trace: could not allocate %ux%u targets", w, h);
    g_w = g_h = 0;
    return false;
  }
  rel(g_vis_rb);
  D3D12_RESOURCE_DESC vd = g_vis.r->GetDesc();
  UINT64 total = 0;
  g_dev->GetCopyableFootprints(&vd, 0, 1, 0, &g_vis_fp, nullptr, nullptr, &total);
  g_vis_rb = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  g_w = w;
  g_h = h;
  TB_LOG("rt trace: targets %ux%u", w, h);
  return true;
}

// Descriptor set i of a slot: SRV t1..t8 then UAV u0..u3. Unused entries get null descriptors.
struct Set {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};
Set set_at(int slot, int i) {
  Set s;
  UINT idx = UINT((slot * kSets + i) * kPerSet);
  s.cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
  s.cpu.ptr += SIZE_T(idx) * g_inc;
  s.gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
  s.gpu.ptr += UINT64(idx) * g_inc;
  return s;
}
void srv(const Set& s, int reg, ID3D12Resource* r, DXGI_FORMAT f) {
  D3D12_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = f;
  d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  d.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE h = s.cpu;
  h.ptr += SIZE_T(reg - 1) * g_inc;
  g_dev->CreateShaderResourceView(r, &d, h);
}
void uavd(const Set& s, int reg, ID3D12Resource* r, DXGI_FORMAT f, bool buf = false) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
  d.Format = f;
  if (buf) {
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = 64;
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  } else {
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE h = s.cpu;
  h.ptr += SIZE_T(8 + reg) * g_inc;
  g_dev->CreateUnorderedAccessView(r, nullptr, &d, h);
}
// Fill a set: srvs[k] for t(k+1), uavs[k] for u(k); nullptr = null descriptor.
struct Bind {
  ID3D12Resource* t[8] = {};
  DXGI_FORMAT tf[8] = {};
  ID3D12Resource* u[4] = {};
  DXGI_FORMAT uf[4] = {};
};
void fill(const Set& s, const Bind& b) {
  static const DXGI_FORMAT kDefT[8] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                       DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                       DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
  static const DXGI_FORMAT kDefU[4] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                                       DXGI_FORMAT_R8G8B8A8_UNORM};
  for (int i = 0; i < 8; i++) srv(s, i + 1, b.t[i], b.t[i] ? b.tf[i] : kDefT[i]);
  for (int i = 0; i < 4; i++) uavd(s, i, b.u[i], b.u[i] ? b.uf[i] : kDefU[i], i == 2);
}

void ts(ID3D12GraphicsCommandList4* l, int slot, int i) {
  if (!g_qh || i >= kTs) return;
  l->EndQuery(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot * kTs + i));
  if (i + 1 > g_ts_n[slot]) g_ts_n[slot] = i + 1;
}

void m44_mul(const float* a, const float* b, float* o) {  // o = a * b (row-major 4x4), doubles inside
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double v = 0;
      for (int k = 0; k < 4; k++) v += double(a[r * 4 + k]) * double(b[k * 4 + c]);
      o[r * 4 + c] = float(v);
    }
}

}  // namespace

bool trace_init(ID3D12Device5* dev, std::string& err) {
  if (g_dev) return true;
  g_dev = dev;
  auto ser = reinterpret_cast<SerializeRS_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!ser) {
    err = "no D3D12SerializeRootSignature";
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 8;
  ranges[0].BaseShaderRegister = 1;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 4;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 8;
  D3D12_ROOT_PARAMETER p[3]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[2].DescriptorTable.NumDescriptorRanges = 2;
  p[2].DescriptorTable.pDescriptorRanges = ranges;
  D3D12_STATIC_SAMPLER_DESC ss[2]{};
  for (int i = 0; i < 2; i++) {
    ss[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[i].MaxLOD = D3D12_FLOAT32_MAX;
    ss[i].ShaderRegister = UINT(i);
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 3;
  rd.pParameters = p;
  rd.NumStaticSamplers = 2;
  rd.pStaticSamplers = ss;
  ID3DBlob *blob = nullptr, *e = nullptr;
  if (FAILED(ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &e))) {
    err = e ? std::string((const char*)e->GetBufferPointer(), e->GetBufferSize()) : "root signature";
    rel(e);
    return false;
  }
  dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                           (void**)&g_rs);
  rel(blob);
  rel(e);
  if (!g_rs) {
    err = "CreateRootSignature";
    return false;
  }
  std::vector<uint8_t> lib, cs1, cs2;
  std::string lib_src = std::string(kCommon) + kLib, cs_src = std::string(kCommon) + kCs;
  if (!dxc_compile(lib_src.c_str(), L"", L"lib_6_3", lib, err)) {
    err = "lib: " + err;
    return false;
  }
  if (!dxc_compile(cs_src.c_str(), L"cs_temporal", L"cs_6_0", cs1, err) ||
      !dxc_compile(cs_src.c_str(), L"cs_atrous", L"cs_6_0", cs2, err)) {
    err = "cs: " + err;
    return false;
  }
  D3D12_EXPORT_DESC ex[5] = {{L"rg_ao", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"rg_val", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"rg_sun", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"ms", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"ch", nullptr, D3D12_EXPORT_FLAG_NONE}};
  D3D12_DXIL_LIBRARY_DESC ld{};
  ld.DXILLibrary.pShaderBytecode = lib.data();
  ld.DXILLibrary.BytecodeLength = lib.size();
  ld.NumExports = 5;
  ld.pExports = ex;
  D3D12_HIT_GROUP_DESC hg{};
  hg.HitGroupExport = L"hg";
  hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hg.ClosestHitShaderImport = L"ch";
  D3D12_RAYTRACING_SHADER_CONFIG sc{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pc{1};
  D3D12_GLOBAL_ROOT_SIGNATURE grs{g_rs};
  D3D12_STATE_SUBOBJECT sub[5] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld},
                                  {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg},
                                  {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc},
                                  {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc},
                                  {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs}};
  D3D12_STATE_OBJECT_DESC sd{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, sub};
  HRESULT hr = dev->CreateStateObject(&sd, __uuidof(ID3D12StateObject), (void**)&g_so);
  if (FAILED(hr) || !g_so) {
    char b[64];
    snprintf(b, sizeof b, "CreateStateObject hr %#lx", (unsigned long)hr);
    err = b;
    return false;
  }
  ID3D12StateObjectProperties* props = nullptr;
  g_so->QueryInterface(__uuidof(ID3D12StateObjectProperties), (void**)&props);
  uint8_t table[5 * 64] = {};
  const wchar_t* names[5] = {L"rg_ao", L"rg_val", L"rg_sun", L"ms", L"hg"};
  for (int i = 0; i < 5 && props; i++) {
    void* id = props->GetShaderIdentifier(names[i]);
    if (!id) {
      err = "no shader identifier";
      rel(props);
      return false;
    }
    memcpy(table + i * 64, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
  }
  rel(props);
  g_table = buffer(sizeof table, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  void* tp = nullptr;
  if (!g_table || FAILED(g_table->Map(0, nullptr, &tp))) {
    err = "shader table";
    return false;
  }
  memcpy(tp, table, sizeof table);
  g_table->Unmap(0, nullptr);
  for (int k = 0; k < 2; k++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = g_rs;
    auto& code = k ? cs2 : cs1;
    cd.CS.pShaderBytecode = code.data();
    cd.CS.BytecodeLength = code.size();
    if (FAILED(dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState),
                                               (void**)(k ? &g_cs_atrous : &g_cs_temporal)))) {
      err = "compute PSO";
      return false;
    }
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kSlots * kSets * kPerSet;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_heap))) {
    err = "descriptor heap";
    return false;
  }
  g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (int s = 0; s < kSlots; s++) {
    g_cb[s] = buffer(kSets * kCbStride * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE none{0, 0};
    if (!g_cb[s] || FAILED(g_cb[s]->Map(0, &none, (void**)&g_cb_cpu[s]))) {
      err = "constant buffers";
      return false;
    }
    g_stats_rb[s] = buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  g_stats = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = kSlots * kTs;
  dev->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap), (void**)&g_qh);
  g_ts_rb = buffer(kSlots * kTs * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  // Timestamp frequency of the direct queue: one of ours (the device is the game's).
  ID3D12CommandQueue* q = nullptr;
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  if (SUCCEEDED(dev->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
    q->GetTimestampFrequency(&g_ts_freq);
    q->Release();
  }
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{DXGI_FORMAT_B8G8R8A8_UNORM, {}, {}};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
    g_bgra_uav = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
  TB_LOG("rt trace: pipelines ready (lib %zu bytes, cs %zu + %zu), BGRA8 UAV stores %s", lib.size(), cs1.size(),
         cs2.size(), g_bgra_uav ? "yes" : "no");
  return g_stats && g_ts_rb;
}

void trace_mark(ID3D12GraphicsCommandList4* l, int slot, int i) { ts(l, slot, i); }

ID3D12Resource* trace_record(ID3D12GraphicsCommandList4* l, const EyeInputs& in, int slot) {
  if (!g_so || !in.depth || !in.normal) return nullptr;
  bool val, img;
  unsigned vmask;
  {
    std::lock_guard lk(g_mu);
    val = g_val.armed;
    img = g_val.image;
    vmask = g_val.mask;
    g_val.armed = false;
  }
  bool ao = g_ao_on.load(), sun = g_sun_on.load() && g_bgra_uav;
  if (!in.tlas || (!val && !ao && !sun)) return nullptr;
  D3D12_RESOURCE_DESC dd = in.depth->GetDesc();
  if (dd.Format != DXGI_FORMAT_R32_FLOAT && dd.Format != DXGI_FORMAT_R32_TYPELESS) return nullptr;
  if (!targets(uint32_t(dd.Width), dd.Height)) return nullptr;
  // The engine's targets must be in a known state (seen transitioned by its lists).
  if (!in.depth_state || !in.normal_state) {
    g_skipped_state++;
    return nullptr;
  }
  const int e = in.eye ? 1 : 0;
  ts(l, slot, 1);  // after the acceleration structures (scene.cpp marks 0 at the start)
  ID3D12DescriptorHeap* heaps[] = {g_heap};
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootSignature(g_rs);
  l->SetComputeRootShaderResourceView(0, in.tlas);

  // The engine's G-buffer targets: readable by non-pixel shaders, put back at the end.
  D3D12_RESOURCE_BARRIER eb[2]{};
  int neb = 0;
  auto add_eb = [&](ID3D12Resource* r, unsigned st) {
    if (st & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) return;
    eb[neb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    eb[neb].Transition.pResource = r;
    eb[neb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    eb[neb].Transition.StateBefore = D3D12_RESOURCE_STATES(st);
    eb[neb].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    neb++;
  };
  add_eb(in.depth, in.depth_state);
  add_eb(in.normal, in.normal_state);
  if (neb) l->ResourceBarrier(UINT(neb), eb);

  // Constants.
  Cb c{};
  memcpy(c.c2v, in.cam.c2v, 64);
  memcpy(c.v2w, in.cam.v2w, 64);
  c.v2w[3] = float(double(in.cam.v2w[3]) - in.anchor[0]);
  c.v2w[7] = float(double(in.cam.v2w[7]) - in.anchor[1]);
  c.v2w[11] = float(double(in.cam.v2w[11]) - in.anchor[2]);
  c.p0[0] = g_radius;
  c.p0[1] = g_bias;
  c.p0[2] = float(in.frame & 0xffff);
  c.p0[3] = g_hist_len;
  c.p1[0] = float(g_w);
  c.p1[1] = float(g_h);
  c.p1[2] = 1.0f / float(g_w);
  c.p1[3] = 1.0f / float(g_h);
  c.u0[0] = uint32_t(g_mask.load());
  c.u0[1] = uint32_t(std::clamp(g_spp.load(), 1, 64));
  c.u0[2] = 0;
  c.u0[3] = 0;
  float sx = g_sun_dir[0], sy = g_sun_dir[1], sz = g_sun_dir[2];
  float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
  c.sun[0] = sx / (sl > 0 ? sl : 1);
  c.sun[1] = sy / (sl > 0 ? sl : 1);
  c.sun[2] = sz / (sl > 0 ? sl : 1);
  c.sun[3] = g_sun_cone;
  c.p2[0] = g_strength;
  c.p2[1] = 2000.0f;
  c.p2[2] = g_alpha_cov;
  int ncb = 0;
  auto push_cb = [&](const Cb& v) -> D3D12_GPU_VIRTUAL_ADDRESS {
    int i = ncb++ % (kSets * 4);
    memcpy(g_cb_cpu[slot] + i * kCbStride, &v, sizeof v);
    return g_cb[slot]->GetGPUVirtualAddress() + UINT64(i) * kCbStride;
  };
  int nset = 0;
  D3D12_GPU_VIRTUAL_ADDRESS base = g_table->GetGPUVirtualAddress();
  auto rays = [&](int rg) {
    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord = {base + UINT64(rg) * 64, 64};
    dr.MissShaderTable = {base + 3 * 64, 64, 64};
    dr.HitGroupTable = {base + 4 * 64, 64, 64};
    dr.Width = g_w;
    dr.Height = g_h;
    dr.Depth = 1;
    l->SetPipelineState1(g_so);
    l->DispatchRays(&dr);
  };
  auto bind = [&](const Bind& b, const Cb& v) {
    Set s = set_at(slot, nset++ % kSets);
    fill(s, b);
    l->SetComputeRootConstantBufferView(1, push_cb(v));
    l->SetComputeRootDescriptorTable(2, s.gpu);
  };
  ID3D12Resource* result = nullptr;

  // Validation (armed by `rt scene validate`).
  if (val) {
    D3D12_RESOURCE_BARRIER b{};
    Cb v = c;
    v.u0[0] = vmask;
    v.u0[2] = img ? 1 : 0;
    // zero the counters: a copy from a zeroed upload region (the constant ring's tail is unused)
    static ID3D12Resource* zero = nullptr;
    if (!zero) zero = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_stats;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(1, &b);
    if (zero) {
      void* zp = nullptr;
      if (SUCCEEDED(zero->Map(0, nullptr, &zp))) {
        memset(zp, 0, 256);
        zero->Unmap(0, nullptr);
      }
      l->CopyBufferRegion(g_stats, 0, zero, 0, 256);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    to(l, g_vis, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind bd;
    bd.t[0] = in.depth;
    bd.tf[0] = DXGI_FORMAT_R32_FLOAT;
    bd.t[1] = in.normal;
    bd.tf[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bd.u[2] = g_stats;
    bd.u[3] = g_vis.r;
    bd.uf[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
    bind(bd, v);
    rays(1);
    uav(l);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    l->ResourceBarrier(1, &b);
    l->CopyBufferRegion(g_stats_rb[slot], 0, g_stats, 0, 256);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    if (img && g_vis_rb) {
      to(l, g_vis, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = g_vis_rb;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = g_vis_fp;
      src.pResource = g_vis.r;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    g_slot_val[slot] = true;
    g_slot_img[slot] = img;
    {
      std::lock_guard lk(g_mu);
      g_val.frame = in.frame;
    }
  }
  ts(l, slot, 2);

  // A ray pass and its denoiser: kind 0 AO (rg_ao, or rg_sun with [dev] rtao_mode = 2), kind 1
  // sun / moon visibility (rg_sun).
  auto chain = [&](int kind) -> ID3D12Resource* {
    Chain& ch = g_ch[kind][e];
    if (!chain_alloc(ch, kind)) return nullptr;
    int mode = kind ? 0 : g_mode.load();
    int cur = ch.cur ^ 1;
    Tex &hist = ch.hist[cur], &hprev = ch.hist[cur ^ 1], &lin = ch.lin[cur], &lprev = ch.lin[cur ^ 1];
    Cb ck = c;
    bool hist_ok = ch.frame != ~0ull && in.frame - ch.frame <= 2 && mode != 1;
    if (hist_ok) m44_mul(ch.prev_w2c, in.cam.v2w, ck.v2pc);
    ck.u0[3] = hist_ok ? 1 : 0;
    if (kind) {
      ck.u0[0] = uint32_t(g_sun_mask.load());
      ck.u0[1] = uint32_t(std::clamp(g_sun_spp.load(), 1, 64));
    }
    // 1. rays -> raw (value, linear depth)
    to(l, g_raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind b1;
    b1.t[0] = in.depth;
    b1.tf[0] = DXGI_FORMAT_R32_FLOAT;
    b1.t[1] = in.normal;
    b1.tf[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    b1.u[0] = g_raw.r;
    b1.uf[0] = DXGI_FORMAT_R16G16_FLOAT;
    bind(b1, ck);
    rays(kind || mode == 2 ? 2 : 0);
    ts(l, slot, kind ? 5 : 3);
    // 2. temporal -> hist[cur], lin[cur]
    to(l, g_raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    to(l, hprev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    to(l, lprev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    to(l, hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    to(l, lin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind b2;
    b2.t[0] = in.depth;
    b2.tf[0] = DXGI_FORMAT_R32_FLOAT;
    b2.t[2] = g_raw.r;
    b2.tf[2] = DXGI_FORMAT_R16G16_FLOAT;
    b2.t[3] = hprev.r;
    b2.tf[3] = DXGI_FORMAT_R16G16_FLOAT;
    b2.t[4] = lprev.r;
    b2.tf[4] = DXGI_FORMAT_R32_FLOAT;
    b2.u[0] = hist.r;
    b2.uf[0] = DXGI_FORMAT_R16G16_FLOAT;
    b2.u[1] = lin.r;
    b2.uf[1] = DXGI_FORMAT_R32_FLOAT;
    bind(b2, ck);
    l->SetPipelineState(g_cs_temporal);
    l->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
    // 3. a-trous, steps 1, 2, 4: hist -> tmp -> raw (free again) -> the chain's output
    to(l, hist, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    to(l, lin, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Tex* srcs[3] = {&hist, &g_tmp, &g_raw};
    Tex* dsts[3] = {&g_tmp, &g_raw, &ch.out};
    for (int pass = 0; pass < 3; pass++) {
      bool last = pass == 2;
      to(l, *srcs[pass], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      to(l, *dsts[pass], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Bind b3;
      b3.t[0] = in.depth;
      b3.tf[0] = DXGI_FORMAT_R32_FLOAT;
      b3.t[1] = in.normal;
      b3.tf[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
      b3.t[2] = srcs[pass]->r;
      b3.tf[2] = DXGI_FORMAT_R16G16_FLOAT;
      b3.t[4] = lin.r;
      b3.tf[4] = DXGI_FORMAT_R32_FLOAT;
      if (last && kind) {
        b3.u[3] = ch.out.r;
        b3.uf[3] = DXGI_FORMAT_B8G8R8A8_UNORM;
      } else {
        b3.u[0] = dsts[pass]->r;
        b3.uf[0] = DXGI_FORMAT_R16G16_FLOAT;
      }
      Cb c3 = ck;
      c3.u0[1] = (last ? (kind ? 3u : 2u) : 1u) << 8;
      c3.u0[2] = 1u << pass;
      if (mode == 1) c3.u0[2] = 0;  // raw: no spatial filter (step 0 = the pixel itself)
      bind(b3, c3);
      l->SetPipelineState(g_cs_atrous);
      l->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
    }
    to(l, ch.out, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ch.cur = cur;
    ch.frame = in.frame;
    memcpy(ch.prev_w2c, in.cam.w2c, 64);
    ts(l, slot, kind ? 6 : 4);
    return ch.out.r;
  };
  if (ao) result = chain(0);
  if (sun) chain(1);
  // The engine's targets back to their states.
  for (int i = 0; i < neb; i++) std::swap(eb[i].Transition.StateBefore, eb[i].Transition.StateAfter);
  if (neb) l->ResourceBarrier(UINT(neb), eb);
  if (g_qh && g_ts_n[slot]) l->ResolveQueryData(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot * kTs), UINT(g_ts_n[slot]),
                                                g_ts_rb, UINT64(slot * kTs) * 8);
  g_traced++;
  return result;
}

void trace_slot_done(int slot) {
  if (g_ts_rb && g_ts_n[slot] && g_ts_freq) {
    UINT64* p = nullptr;
    D3D12_RANGE rr{size_t(slot * kTs) * 8, size_t(slot * kTs + kTs) * 8};
    if (SUCCEEDED(g_ts_rb->Map(0, &rr, (void**)&p))) {
      UINT64* t = p + slot * kTs;
      std::lock_guard lk(g_mu);
      for (int i = 1; i < g_ts_n[slot]; i++)
        if (t[i] >= t[0] && t[i] - t[0] < g_ts_freq) {  // ms since the list's start mark
          g_tm.sum[i] += double(t[i] - t[0]) * 1000.0 / double(g_ts_freq);
          g_tm.n[i]++;
        }
      D3D12_RANGE none{0, 0};
      g_ts_rb->Unmap(0, &none);
    }
    g_ts_n[slot] = 0;
  }
  if (g_slot_val[slot]) {
    g_slot_val[slot] = false;
    uint32_t* p = nullptr;
    D3D12_RANGE rr{0, 64};
    if (SUCCEEDED(g_stats_rb[slot]->Map(0, &rr, (void**)&p))) {
      std::lock_guard lk(g_mu);
      memcpy(g_val.bins, p, 64);
      g_val.have = true;
      D3D12_RANGE none{0, 0};
      g_stats_rb[slot]->Unmap(0, &none);
    }
    if (g_slot_img[slot] && g_vis_rb) {
      g_slot_img[slot] = false;
      void* vp = nullptr;
      D3D12_RANGE vr{0, size_t(g_vis_fp.Footprint.RowPitch) * g_vis_fp.Footprint.Height};
      std::string path = out_dir() + "\\rt-validate.raw";
      if (SUCCEEDED(g_vis_rb->Map(0, &vr, &vp))) {
        FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"wb");
        if (f) {
          uint32_t h[5] = {0x54524254u, g_w, g_h, uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM), g_vis_fp.Footprint.RowPitch};
          fwrite(h, 4, 5, f);
          fwrite(vp, 1, vr.End, f);
          fclose(f);
        }
        D3D12_RANGE none{0, 0};
        g_vis_rb->Unmap(0, &none);
        std::lock_guard lk(g_mu);
        g_val.image_path = path;
      }
    }
  }
}

void trace_validate_arm(unsigned mask, bool image) {
  std::lock_guard lk(g_mu);
  g_val.armed = true;
  g_val.image = image;
  g_val.mask = mask;
  g_val.have = false;
}

ID3D12Resource* trace_ao_result(int eye, uint64_t frame) {
  Chain& c = g_ch[0][eye ? 1 : 0];
  return g_ao_on.load() && c.frame == frame ? c.out.r : nullptr;
}

ID3D12Resource* trace_sun_result(int eye, uint64_t frame) {
  Chain& c = g_ch[1][eye ? 1 : 0];
  return g_sun_on.load() && c.frame == frame ? c.out.r : nullptr;
}

void trace_set_sun(const float d[3]) {
  if (g_sun_override.load()) return;
  for (int i = 0; i < 3; i++) g_sun_dir[i] = d[i];
}

std::string trace_status() {
  std::lock_guard lk(g_mu);
  std::string s = "{\"traced\":" + std::to_string(g_traced) + ",\"skipped_state\":" + std::to_string(g_skipped_state) +
                  ",\"size\":[" + std::to_string(g_w) + "," + std::to_string(g_h) + "]";
  static const char* kTsName[kTs] = {"start", "as_done", "validate_done", "ao_rays_done", "ao_denoise_done",
                                     "sun_rays_done", "sun_denoise_done"};
  s += ",\"gpu_ms_since_start\":{";
  bool first = true;
  for (int i = 1; i < kTs; i++) {
    if (!g_tm.n[i] || !kTsName[i]) continue;
    char b[96];
    snprintf(b, sizeof b, "%s\"%s\":%.4f", first ? "" : ",", kTsName[i], g_tm.sum[i] / double(g_tm.n[i]));
    s += b;
    first = false;
  }
  s += "}";
  if (g_val.have) {
    uint64_t tot = 0;
    for (uint32_t v : g_val.bins) tot += v;
    s += ",\"validate\":{\"frame\":" + std::to_string(g_val.frame) + ",\"mask\":" + std::to_string(g_val.mask) +
         ",\"pixels\":" + std::to_string(tot) + ",\"bins\":[";
    for (int i = 0; i < 16; i++) s += (i ? "," : "") + std::to_string(g_val.bins[i]);
    s += "],\"image\":" + json_str(g_val.image_path) + "}";
  }
  char b[256];
  snprintf(b, sizeof b,
           ",\"params\":{\"radius\":%.2f,\"spp\":%d,\"mask\":%d,\"strength\":%.2f,\"history\":%.0f,\"mode\":%d,"
           "\"sun\":[%.3f,%.3f,%.3f],\"cone\":%.4f,\"sun_on\":%d,\"ao_on\":%d}",
           g_radius.load(), g_spp.load(), g_mask.load(), g_strength.load(), g_hist_len.load(), g_mode.load(),
           g_sun_dir[0].load(), g_sun_dir[1].load(), g_sun_dir[2].load(), g_sun_cone.load(), g_sun_on.load() ? 1 : 0,
           g_ao_on.load() ? 1 : 0);
  return s + b + "}";
}

void trace_timing_reset() {
  std::lock_guard lk(g_mu);
  g_tm = Timing{};
}

// [dev] rtao_* tuning (live).
void trace_config(const std::string& id, const std::string& v) {
  double x = atof(v.c_str());
  if (id == "dev.rtao_radius" && x > 0) g_radius = float(x);
  else if (id == "dev.rtao_spp" && x >= 1) g_spp = int(x);
  else if (id == "dev.rtao_mask" && !v.empty()) g_mask = int(strtol(v.c_str(), nullptr, 0));
  else if (id == "dev.rtao_strength" && x > 0) g_strength = float(x);
  else if (id == "dev.rtao_history" && x >= 1) g_hist_len = float(x);
  else if (id == "dev.rtao_mode" && !v.empty()) g_mode = int(x);
  else if (id == "dev.rtao_bias" && x >= 0 && !v.empty()) g_bias = float(x);
  else if (id == "dev.rt_sun_cone" && x >= 0 && !v.empty()) g_sun_cone = float(x);
  else if (id == "dev.rt_sun") g_sun_on = x != 0;
  else if (id == "dev.rt_sun_spp" && x >= 1) g_sun_spp = int(x);
  else if (id == "dev.rt_sun_mask" && !v.empty()) g_sun_mask = int(strtol(v.c_str(), nullptr, 0));
  else if (id == "dev.rtao_ao") g_ao_on = v != "0";
  else if (id == "dev.rtao_alpha" && !v.empty()) g_alpha_cov = float(x);
  else if (id == "dev.rt_sun_dir") {
    float a = 0, b = 0, c = 0;
    g_sun_override = sscanf(v.c_str(), "%f,%f,%f", &a, &b, &c) == 3;
    if (g_sun_override) {
      g_sun_dir[0] = a;
      g_sun_dir[1] = b;
      g_sun_dir[2] = c;
    }
  }
}

}  // namespace tb::rt
