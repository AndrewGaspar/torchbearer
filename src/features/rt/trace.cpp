// Ray-traced passes on the scene's TLAS (rt_scene.h, docs/raytracing.md sections 9 and 13):
//  - cs_val: primary rays from the camera, hit distance against the G-buffer depth (validation:
//    a histogram of the relative error, optionally a colour-coded image);
//  - cs_ao: short ambient-occlusion rays (cosine-weighted around the G-buffer normal, first hit
//    ends), 1..N per pixel, and the receiver's TLAS instance;
//  - cs_temporal: accumulation with this eye's own history, reprojected through each pixel's
//    instance motion and the eye's previous camera (the engine's motion vectors are written after
//    the lighting, so not yet available at this point of the frame), rejected on a linear-depth
//    mismatch;
//  - cs_atrous x3: depth/normal-aware spatial filter; the last one writes the result in the
//    format of the engine's HBAO buffer (R16G16_FLOAT, AO in R), which scene.cpp copies into
//    g_sHBAOBuffer right before the engine's HBAO apply puts it into pShadowRT.a;
//  - cs_gather: per update, the UVs of the alpha-tested triangles (scene.cpp's jobs).
// All are compute shaders with inline ray queries (cs_6_5, shaders/rays.hlsl), compiled at dev
// time with DXC 1.9 and embedded (rays_dxil.h): the DLL carries only the mod's own DXIL.
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
#include "features/rt/rt_internal.h"
#include "features/rt/rt_scene.h"
#include "features/rt/shaders/rays_dxil.h"

namespace tb::rt {
namespace {

constexpr int kSlots = 4;
constexpr int kSets = 16;     // descriptor sets per slot
constexpr int kPerSet = 14;   // t1..t8, u0..u5
constexpr int kSlotDesc = kSets * kPerSet + kGatherBufs + kAlphaTex;  // per slot: sets, gather buffers, textures
constexpr int kTs = 16;       // timestamps per slot
constexpr int kCbStride = 512;  // constant-buffer entries (sizeof(Cb), 256-aligned)

struct Cb {
  float c2v[16], v2w[16], w2pc[16];
  float p0[4], p1[4];
  uint32_t u0[4];
  float sun[4], p2[4];
  uint32_t u1[4];
};

static_assert(sizeof(Cb) <= kCbStride, "constant entries overlap");

template <typename T>
void rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

ID3D12Device5* g_dev = nullptr;
ID3D12RootSignature* g_rs = nullptr;
enum Cs { CS_GATHER, CS_AO, CS_SUN, CS_VAL, CS_TEMPORAL, CS_ATROUS, CS_COUNT };
ID3D12PipelineState* g_cs[CS_COUNT] = {};
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
ID3D12Resource* g_cb[kSlots] = {};
uint8_t* g_cb_cpu[kSlots] = {};
ID3D12Resource* g_dummy = nullptr;  // a small buffer for root SRVs a pass does not use
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
Tex g_raw, g_tmp, g_vis, g_inst;
// Per kind (0 AO, 1 sun / moon visibility) and eye: history ping-pong, linear depth ping-pong,
// the final result (AO: R16G16_FLOAT like g_sHBAOBuffer; sun: B8G8R8A8 like pShadowRT).
struct Chain {
  Tex hist[2], lin[2], out;
  int cur = 0;
  uint64_t frame = ~0ull;      // last traced update
  double prev_w2c[16] = {};
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
  bool armed = false, image = false, alpha = true, ran_alpha = true;
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
uint64_t g_traced = 0, g_skipped_state = 0, g_gathers = 0;

// Config (graphics.cpp sets [graphics] ao; the [dev] rtao_* keys tune it).
std::atomic<float> g_radius{1.5f}, g_strength{1.0f}, g_bias{0.02f}, g_hist_len{24.f}, g_recv_tol{0.03f};
std::atomic<int> g_spp{1}, g_mask{0x0f}, g_mode{0};  // mode: 0 AO, 1 raw AO, 2 sun visibility
std::atomic<bool> g_alpha_on{true}, g_motion_on{true}, g_recv_hit{true};
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
            tex(g_vis, DXGI_FORMAT_R8G8B8A8_UNORM, w, h) && tex(g_inst, DXGI_FORMAT_R32_UINT, w, h);
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

// Descriptor set i of a slot: SRV t1..t8 then UAV u0..u5. Unused entries get null descriptors.
struct Set {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};
Set at_index(UINT idx) {
  Set s;
  s.cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
  s.cpu.ptr += SIZE_T(idx) * g_inc;
  s.gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
  s.gpu.ptr += UINT64(idx) * g_inc;
  return s;
}
Set set_at(int slot, int i) { return at_index(UINT(slot * kSlotDesc + i * kPerSet)); }
Set bufs_at(int slot) { return at_index(UINT(slot * kSlotDesc + kSets * kPerSet)); }
Set texs_at(int slot) { return at_index(UINT(slot * kSlotDesc + kSets * kPerSet + kGatherBufs)); }
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
// Fill a set: t[k] for t(k+1), u[k] for u(k); nullptr = null descriptor.
struct Bind {
  ID3D12Resource* t[8] = {};
  DXGI_FORMAT tf[8] = {};
  ID3D12Resource* u[6] = {};
  DXGI_FORMAT uf[6] = {};
};
void fill(const Set& s, const Bind& b) {
  static const DXGI_FORMAT kDefT[8] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                       DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT,
                                       DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
  static const DXGI_FORMAT kDefU[6] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT};
  for (int i = 0; i < 8; i++) srv(s, i + 1, b.t[i], b.t[i] ? b.tf[i] : kDefT[i]);
  for (int i = 0; i < 6; i++) uavd(s, i, b.u[i], b.u[i] ? b.uf[i] : kDefU[i], i == 2);
}

void ts(ID3D12GraphicsCommandList4* l, int slot, int i) {
  if (!g_qh || i >= kTs) return;
  l->EndQuery(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, UINT(slot * kTs + i));
  if (i + 1 > g_ts_n[slot]) g_ts_n[slot] = i + 1;
}

// o = a (absolute world -> clip, 4x4) * translate(anchor): anchor-relative world -> clip.
void w2c_rel(const double* a, const double* anchor, float* o) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) o[r * 4 + c] = float(a[r * 4 + c]);
    o[r * 4 + 3] = float(a[r * 4] * anchor[0] + a[r * 4 + 1] * anchor[1] + a[r * 4 + 2] * anchor[2] + a[r * 4 + 3]);
  }
}

// Root parameters (trace_init).
enum Rp { RP_TLAS, RP_CB, RP_SET, RP_ALPHA, RP_ALPHA_TEX, RP_MOTION, RP_BUFS, RP_JOBS, RP_UV, RP_COUNT };

void bind_common(ID3D12GraphicsCommandList4* l) {
  ID3D12DescriptorHeap* heaps[] = {g_heap};
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootSignature(g_rs);
  uint64_t d = g_dummy->GetGPUVirtualAddress();
  // Every root parameter set (a pass leaves the ones it does not read on a dummy).
  l->SetComputeRootShaderResourceView(RP_ALPHA, d);
  l->SetComputeRootShaderResourceView(RP_MOTION, d);
  l->SetComputeRootShaderResourceView(RP_JOBS, d);
  l->SetComputeRootUnorderedAccessView(RP_UV, d);
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
  D3D12_DESCRIPTOR_RANGE ranges[4]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 1, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0, 0, 8};
  ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT(kAlphaTex), 0, 1, 0};
  ranges[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT(kGatherBufs), 0, 2, 0};
  D3D12_ROOT_PARAMETER p[RP_COUNT]{};
  p[RP_TLAS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[RP_TLAS].Descriptor = {0, 0};
  p[RP_CB].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  p[RP_CB].Descriptor = {0, 0};
  p[RP_SET].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[RP_SET].DescriptorTable = {2, ranges};
  p[RP_ALPHA].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[RP_ALPHA].Descriptor = {9, 0};
  p[RP_ALPHA_TEX].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[RP_ALPHA_TEX].DescriptorTable = {1, &ranges[2]};
  p[RP_MOTION].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[RP_MOTION].Descriptor = {10, 0};
  p[RP_BUFS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[RP_BUFS].DescriptorTable = {1, &ranges[3]};
  p[RP_JOBS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[RP_JOBS].Descriptor = {11, 0};
  p[RP_UV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[RP_UV].Descriptor = {6, 0};
  D3D12_STATIC_SAMPLER_DESC ss[3]{};
  for (int i = 0; i < 3; i++) {
    ss[i].Filter = i == 1 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[i].MaxLOD = D3D12_FLOAT32_MAX;
    ss[i].ShaderRegister = UINT(i);
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = RP_COUNT;
  rd.pParameters = p;
  rd.NumStaticSamplers = 3;
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
  static const struct {
    const unsigned char* code;
    size_t n;
  } kCode[CS_COUNT] = {{kCsGatherDxil, sizeof kCsGatherDxil}, {kCsAoDxil, sizeof kCsAoDxil},
                       {kCsSunDxil, sizeof kCsSunDxil},       {kCsValDxil, sizeof kCsValDxil},
                       {kCsTemporalDxil, sizeof kCsTemporalDxil}, {kCsAtrousDxil, sizeof kCsAtrousDxil}};
  for (int k = 0; k < CS_COUNT; k++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = g_rs;
    cd.CS.pShaderBytecode = kCode[k].code;
    cd.CS.BytecodeLength = kCode[k].n;
    HRESULT hr = dev->CreateComputePipelineState(&cd, __uuidof(ID3D12PipelineState), (void**)&g_cs[k]);
    if (FAILED(hr)) {
      char b[64];
      snprintf(b, sizeof b, "compute PSO %d hr %#lx", k, (unsigned long)hr);
      err = b;
      return false;
    }
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kSlots * kSlotDesc;
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
  g_dummy = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
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
  TB_LOG("rt trace: ray-query pipelines ready (%zu bytes of DXIL), BGRA8 UAV stores %s",
         sizeof kCsGatherDxil + sizeof kCsAoDxil + sizeof kCsSunDxil + sizeof kCsValDxil + sizeof kCsTemporalDxil +
             sizeof kCsAtrousDxil,
         g_bgra_uav ? "yes" : "no");
  return g_stats && g_ts_rb && g_dummy;
}

void trace_mark(ID3D12GraphicsCommandList4* l, int slot, int i) { ts(l, slot, i); }

bool trace_alpha_heap(int slot, uint64_t* tex_cpu, uint64_t* buf_cpu, unsigned* inc) {
  if (!g_heap || slot < 0 || slot >= kSlots) return false;
  *tex_cpu = texs_at(slot).cpu.ptr;
  *buf_cpu = bufs_at(slot).cpu.ptr;
  *inc = g_inc;
  return true;
}

void trace_gather(ID3D12GraphicsCommandList4* l, int slot, uint64_t jobs, uint32_t groups_off, uint32_t ngroups,
                  uint64_t uv) {
  if (!g_cs[CS_GATHER] || !ngroups) return;
  bind_common(l);
  Cb c{};
  c.u1[3] = groups_off;
  int i = kSets * 4 - 1;  // the constant ring's last entry (trace_record uses the others)
  memcpy(g_cb_cpu[slot] + i * kCbStride, &c, sizeof c);
  l->SetComputeRootConstantBufferView(RP_CB, g_cb[slot]->GetGPUVirtualAddress() + UINT64(i) * kCbStride);
  l->SetComputeRootDescriptorTable(RP_BUFS, bufs_at(slot).gpu);
  l->SetComputeRootShaderResourceView(RP_JOBS, jobs);
  l->SetComputeRootUnorderedAccessView(RP_UV, uv);
  l->SetPipelineState(g_cs[CS_GATHER]);
  for (uint32_t g = 0; g < ngroups; g += 65535) {
    // Groups beyond 65535: a second dispatch with its own group offset.
    if (g) {
      Cb c2 = c;
      c2.u1[3] = groups_off + g * 8;
      int j = kSets * 4 - 2;
      memcpy(g_cb_cpu[slot] + j * kCbStride, &c2, sizeof c2);
      l->SetComputeRootConstantBufferView(RP_CB, g_cb[slot]->GetGPUVirtualAddress() + UINT64(j) * kCbStride);
    }
    l->Dispatch(std::min<uint32_t>(ngroups - g, 65535), 1, 1);
  }
  g_gathers++;
}

ID3D12Resource* trace_record(ID3D12GraphicsCommandList4* l, const EyeInputs& in, int slot) {
  if (!g_cs[CS_AO] || !in.depth || !in.normal) return nullptr;
  bool val, img, valpha;
  unsigned vmask;
  {
    std::lock_guard lk(g_mu);
    val = g_val.armed;
    img = g_val.image;
    vmask = g_val.mask;
    valpha = g_val.alpha;
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
  bind_common(l);
  l->SetComputeRootShaderResourceView(RP_TLAS, in.tlas);
  bool have_alpha = in.alpha && in.alpha_slot >= 0;
  if (have_alpha) {
    l->SetComputeRootShaderResourceView(RP_ALPHA, in.alpha);
    l->SetComputeRootDescriptorTable(RP_ALPHA_TEX, texs_at(in.alpha_slot).gpu);
  } else {
    l->SetComputeRootDescriptorTable(RP_ALPHA_TEX, texs_at(slot).gpu);
  }
  l->SetComputeRootDescriptorTable(RP_BUFS, bufs_at(slot).gpu);
  if (in.motion) l->SetComputeRootShaderResourceView(RP_MOTION, in.motion);

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
  float sx = g_sun_dir[0], sy = g_sun_dir[1], sz = g_sun_dir[2];
  float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
  c.sun[0] = sx / (sl > 0 ? sl : 1);
  c.sun[1] = sy / (sl > 0 ? sl : 1);
  c.sun[2] = sz / (sl > 0 ? sl : 1);
  c.sun[3] = g_sun_cone;
  c.p2[0] = g_strength;
  c.p2[1] = 2000.0f;
  c.p2[2] = g_recv_tol;
  c.u1[0] = g_motion_on.load() && in.motion ? 1 : 0;
  c.u1[1] = g_alpha_on.load() && have_alpha ? 1 : 0;
  c.u1[2] = g_recv_hit.load() ? 1 : 0;
  int ncb = 0;
  auto push_cb = [&](const Cb& v) -> D3D12_GPU_VIRTUAL_ADDRESS {
    int i = ncb++ % (kSets * 4 - 2);
    memcpy(g_cb_cpu[slot] + i * kCbStride, &v, sizeof v);
    return g_cb[slot]->GetGPUVirtualAddress() + UINT64(i) * kCbStride;
  };
  int nset = 0;
  auto bind = [&](const Bind& b, const Cb& v) {
    Set s = set_at(slot, nset++ % kSets);
    fill(s, b);
    l->SetComputeRootConstantBufferView(RP_CB, push_cb(v));
    l->SetComputeRootDescriptorTable(RP_SET, s.gpu);
  };
  auto run = [&](Cs k) {
    l->SetPipelineState(g_cs[k]);
    l->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
  };
  ID3D12Resource* result = nullptr;

  // Validation (armed by `rt scene validate`).
  if (val) {
    D3D12_RESOURCE_BARRIER b{};
    Cb v = c;
    v.u0[0] = vmask;
    v.u0[2] = img ? 1 : 0;
    v.u1[1] = valpha && have_alpha ? 1 : 0;
    // zero the counters: a copy from a zeroed upload buffer
    static ID3D12Resource* zero = nullptr;
    if (!zero) {
      zero = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
      void* zp = nullptr;
      if (zero && SUCCEEDED(zero->Map(0, nullptr, &zp))) {
        memset(zp, 0, 256);
        zero->Unmap(0, nullptr);
      }
    }
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_stats;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(1, &b);
    if (zero) l->CopyBufferRegion(g_stats, 0, zero, 0, 256);
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
    run(CS_VAL);
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
      g_val.ran_alpha = v.u1[1] != 0;
    }
  }
  ts(l, slot, 2);

  // A ray pass and its denoiser: kind 0 AO (cs_ao, or cs_sun with [dev] rtao_mode = 2), kind 1
  // sun / moon visibility (cs_sun).
  auto chain = [&](int kind) -> ID3D12Resource* {
    Chain& ch = g_ch[kind][e];
    if (!chain_alloc(ch, kind)) return nullptr;
    int mode = kind ? 0 : g_mode.load();
    int cur = ch.cur ^ 1;
    Tex &hist = ch.hist[cur], &hprev = ch.hist[cur ^ 1], &lin = ch.lin[cur], &lprev = ch.lin[cur ^ 1];
    Cb ck = c;
    bool hist_ok = ch.frame != ~0ull && in.frame - ch.frame <= 2 && mode != 1;
    if (hist_ok) w2c_rel(ch.prev_w2c, in.anchor, ck.w2pc);
    ck.u0[3] = hist_ok ? 1 : 0;
    if (kind) {
      ck.u0[0] = uint32_t(g_sun_mask.load());
      ck.u0[1] = uint32_t(std::clamp(g_sun_spp.load(), 1, 64));
    }
    bool sunrays = kind || mode == 2;
    // 1. rays -> raw (value, linear depth), receiver instances
    to(l, g_raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    to(l, g_inst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Bind b1;
    b1.t[0] = in.depth;
    b1.tf[0] = DXGI_FORMAT_R32_FLOAT;
    b1.t[1] = in.normal;
    b1.tf[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    b1.u[0] = g_raw.r;
    b1.uf[0] = DXGI_FORMAT_R16G16_FLOAT;
    b1.u[4] = g_inst.r;
    b1.uf[4] = DXGI_FORMAT_R32_UINT;
    bind(b1, ck);
    run(sunrays ? CS_SUN : CS_AO);
    ts(l, slot, kind ? 5 : 3);
    // 2. temporal -> hist[cur], lin[cur]
    to(l, g_raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    to(l, g_inst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
    if (!sunrays) {  // cs_sun writes no instances: camera-only reprojection
      b2.t[5] = g_inst.r;
      b2.tf[5] = DXGI_FORMAT_R32_UINT;
    }
    b2.u[0] = hist.r;
    b2.uf[0] = DXGI_FORMAT_R16G16_FLOAT;
    b2.u[1] = lin.r;
    b2.uf[1] = DXGI_FORMAT_R32_FLOAT;
    Cb ct = ck;
    if (sunrays) ct.u1[0] = 0;
    bind(b2, ct);
    run(CS_TEMPORAL);
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
      run(CS_ATROUS);
    }
    to(l, ch.out, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ch.cur = cur;
    ch.frame = in.frame;
    memcpy(ch.prev_w2c, in.w2c, sizeof ch.prev_w2c);
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

void trace_validate_arm(unsigned mask, bool image, bool alpha) {
  std::lock_guard lk(g_mu);
  g_val.armed = true;
  g_val.image = image;
  g_val.mask = mask;
  g_val.alpha = alpha;
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
                  ",\"gathers\":" + std::to_string(g_gathers) + ",\"size\":[" + std::to_string(g_w) + "," +
                  std::to_string(g_h) + "]";
  static const char* kTsName[kTs] = {"start", "as_done", "validate_done", "ao_rays_done", "ao_denoise_done",
                                     "sun_rays_done", "sun_denoise_done", "gather_done"};
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
         ",\"alpha\":" + (g_val.ran_alpha ? "true" : "false") + ",\"pixels\":" + std::to_string(tot) + ",\"bins\":[";
    for (int i = 0; i < 16; i++) s += (i ? "," : "") + std::to_string(g_val.bins[i]);
    s += "],\"image\":" + json_str(g_val.image_path) + "}";
  }
  char b[320];
  snprintf(b, sizeof b,
           ",\"params\":{\"radius\":%.2f,\"spp\":%d,\"mask\":%d,\"strength\":%.2f,\"history\":%.0f,\"mode\":%d,"
           "\"alpha\":%d,\"motion\":%d,\"receiver_hit\":%d,\"sun\":[%.3f,%.3f,%.3f],\"cone\":%.4f,\"sun_on\":%d,\"ao_on\":%d}",
           g_radius.load(), g_spp.load(), g_mask.load(), g_strength.load(), g_hist_len.load(), g_mode.load(),
           g_alpha_on.load() ? 1 : 0, g_motion_on.load() ? 1 : 0, g_recv_hit.load() ? 1 : 0, g_sun_dir[0].load(),
           g_sun_dir[1].load(), g_sun_dir[2].load(), g_sun_cone.load(), g_sun_on.load() ? 1 : 0, g_ao_on.load() ? 1 : 0);
  return s + b + "}";
}

void trace_timing_reset() {
  std::lock_guard lk(g_mu);
  g_tm = Timing{};
}

// [dev] rtao_* / rt_* tuning (live).
void trace_config(const std::string& id, const std::string& v) {
  double x = atof(v.c_str());
  if (id == "dev.rtao_radius" && x > 0) g_radius = float(x);
  else if (id == "dev.rtao_spp" && x >= 1) g_spp = int(x);
  else if (id == "dev.rtao_mask" && !v.empty()) g_mask = int(strtol(v.c_str(), nullptr, 0));
  else if (id == "dev.rtao_strength" && x > 0) g_strength = float(x);
  else if (id == "dev.rtao_history" && x >= 1) g_hist_len = float(x);
  else if (id == "dev.rtao_mode" && !v.empty()) g_mode = int(x);
  else if (id == "dev.rtao_bias" && x >= 0 && !v.empty()) g_bias = float(x);
  else if (id == "dev.rtao_motion" && !v.empty()) g_motion_on = x != 0;
  else if (id == "dev.rtao_receiver" && !v.empty()) g_recv_hit = x != 0;
  else if (id == "dev.rtao_receiver_tol" && x > 0) g_recv_tol = float(x);
  else if (id == "dev.rt_alpha" && !v.empty()) g_alpha_on = x != 0;
  else if (id == "dev.rt_sun_cone" && x >= 0 && !v.empty()) g_sun_cone = float(x);
  else if (id == "dev.rt_sun") g_sun_on = x != 0;
  else if (id == "dev.rt_sun_spp" && x >= 1) g_sun_spp = int(x);
  else if (id == "dev.rt_sun_mask" && !v.empty()) g_sun_mask = int(strtol(v.c_str(), nullptr, 0));
  else if (id == "dev.rtao_ao") g_ao_on = v != "0";
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
