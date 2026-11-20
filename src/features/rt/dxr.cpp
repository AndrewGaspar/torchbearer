// DXR probe (docs/raytracing.md, question 1): what the game's D3D12 device reports, and a live
// test that builds acceleration structures and traces rays on that device, on a queue of our own.
// Dev only: runs when `quiver lab call <i> rt dxr [test]` asks; nothing here touches the game's
// queue, lists or resources.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/lab.h"
#include "core/log.h"
#include "features/rt/dxc.h"
#include "features/rt/rt_internal.h"
#include "vkd3d-proton-tests/rayquery_dxil.h"

namespace tb::rt {
namespace {

template <typename T>
void rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::string fmt(const char* f, ...) __attribute__((format(gnu_printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char b[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

const char* sm_name(D3D_SHADER_MODEL m) {
  switch (m) {
    case D3D_SHADER_MODEL_5_1: return "5.1";
    case D3D_SHADER_MODEL_6_0: return "6.0";
    case D3D_SHADER_MODEL_6_1: return "6.1";
    case D3D_SHADER_MODEL_6_2: return "6.2";
    case D3D_SHADER_MODEL_6_3: return "6.3";
    case D3D_SHADER_MODEL_6_4: return "6.4";
    case D3D_SHADER_MODEL_6_5: return "6.5";
    case D3D_SHADER_MODEL_6_6: return "6.6";
    case D3D_SHADER_MODEL_6_7: return "6.7";
    default: return "?";
  }
}

// ---- GPU helpers on our own queue --------------------------------------------------------------

struct Gpu {
  ID3D12Device5* dev = nullptr;
  ID3D12CommandQueue* q = nullptr;
  ID3D12CommandAllocator* a = nullptr;
  ID3D12GraphicsCommandList4* l = nullptr;
  ID3D12Fence* f = nullptr;
  HANDLE ev = nullptr;
  UINT64 fv = 0;
  ID3D12QueryHeap* qh = nullptr;
  ID3D12Resource* qrb = nullptr;  // timestamp readback
  UINT64 freq = 0;
  std::vector<ID3D12Resource*> keep;
  ~Gpu() {
    for (auto* r : keep) r->Release();
    rel(qrb);
    rel(qh);
    rel(l);
    rel(a);
    rel(q);
    rel(f);
    if (ev) CloseHandle(ev);
  }
  bool init(ID3D12Device5* d) {
    dev = d;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q))) return false;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a)))
      return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, __uuidof(ID3D12GraphicsCommandList4),
                                      (void**)&l)))
      return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&f))) return false;
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_QUERY_HEAP_DESC hd{};
    hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = 64;
    if (FAILED(dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), (void**)&qh))) return false;
    qrb = buffer(64 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, false);
    q->GetTimestampFrequency(&freq);
    return qrb != nullptr;
  }
  ID3D12Resource* buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                         bool track = true) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size < 256 ? 256 : size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, __uuidof(ID3D12Resource),
                                            (void**)&r)))
      return nullptr;
    if (track) keep.push_back(r);
    return r;
  }
  ID3D12Resource* upload(const void* data, size_t n) {
    ID3D12Resource* r = buffer(n, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!r) return nullptr;
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(r->Map(0, &none, &p))) return nullptr;
    memcpy(p, data, n);
    r->Unmap(0, nullptr);
    return r;
  }
  void uav_barrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    l->ResourceBarrier(1, &b);
  }
  void ts(UINT i) { l->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, i); }
  // Close, execute, wait; reopen for more. False on timeout / failure.
  bool run(DWORD timeout_ms = 10000) {
    l->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 64, qrb, 0);
    if (FAILED(l->Close())) return false;
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    q->Signal(f, ++fv);
    if (f->GetCompletedValue() < fv) {
      f->SetEventOnCompletion(fv, ev);
      if (WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0) return false;
    }
    a->Reset();
    l->Reset(a, nullptr);
    return true;
  }
  double ms(UINT i0, UINT i1) {
    UINT64* p = nullptr;
    D3D12_RANGE rr{0, 64 * 8};
    if (FAILED(qrb->Map(0, &rr, (void**)&p))) return -1;
    double r = freq ? double(p[i1] - p[i0]) * 1000.0 / double(freq) : -1;
    D3D12_RANGE none{0, 0};
    qrb->Unmap(0, &none);
    return r;
  }
  template <typename T>
  bool read(ID3D12Resource* rb, std::vector<T>& out, size_t n) {
    T* p = nullptr;
    D3D12_RANGE rr{0, n * sizeof(T)};
    if (FAILED(rb->Map(0, &rr, (void**)&p))) return false;
    out.assign(p, p + n);
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    return true;
  }
};

using SerializeRS_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                       ID3DBlob**);

// Root signature for every test shader: t0 = TLAS (root SRV), u0 = output (root UAV), b0 = 4 constants.
ID3D12RootSignature* make_rs(ID3D12Device* dev, std::string& err) {
  auto ser = reinterpret_cast<SerializeRS_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!ser) {
    err = "no D3D12SerializeRootSignature";
    return nullptr;
  }
  D3D12_ROOT_PARAMETER p[3]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  p[2].Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC d{};
  d.NumParameters = 3;
  d.pParameters = p;
  ID3DBlob *blob = nullptr, *e = nullptr;
  if (FAILED(ser(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &e))) {
    err = e ? std::string((const char*)e->GetBufferPointer(), e->GetBufferSize()) : "serialize failed";
    rel(e);
    return nullptr;
  }
  ID3D12RootSignature* rs = nullptr;
  dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&rs);
  rel(blob);
  rel(e);
  if (!rs) err = "CreateRootSignature failed";
  return rs;
}

// DXR 1.0 pipeline: the same rays through raygen / closest hit / miss, results in Out[4..7].
const char* kLibHlsl = R"(
RaytracingAccelerationStructure AS : register(t0);
RWStructuredBuffer<float> Out : register(u0);
struct P { float t; };
[shader("raygeneration")] void rg() {
  uint i = DispatchRaysIndex().x;
  RayDesc r;
  r.Origin = float3(i == 0 ? 0.25 : 5.0, 0.25, -1.0);
  r.Direction = float3(0, 0, 1);
  r.TMin = 0;
  r.TMax = 100;
  P p;
  p.t = -2;
  TraceRay(AS, RAY_FLAG_FORCE_OPAQUE, 0xff, 0, 1, 0, r, p);
  Out[4 + i] = p.t;
}
[shader("closesthit")] void ch(inout P p, in BuiltInTriangleIntersectionAttributes a) { p.t = RayTCurrent(); }
[shader("miss")] void ms(inout P p) { p.t = -1; }
)";

// Throughput (a DXR 1.0 raygen, so the game's DXC 1.4 compiles it): W x H pixels, SPP rays each,
// cosine-distributed downwards from 2 m above a field of instanced bumpy sheets; runtime ray flags
// in C.FL (bit 16: long rays, 1000 m instead of 3 m). Counts hits (one atomic per wave).
const char* kRateHlsl = R"(
RaytracingAccelerationStructure AS : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) { uint W; uint H; uint SPP; uint FL; };
struct P { uint hit; };
uint hsh(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
[shader("raygeneration")] void rg() {
  uint2 p = DispatchRaysIndex().xy;
  uint hits = 0;
  float3 o = float3((p.x + 0.5) / W * 300.0 + 10.0, 2.0, (p.y + 0.5) / H * 300.0 + 10.0);
  float tmax = (FL & 0x10000) ? 1000.0 : 3.0;
  for (uint s = 0; s < SPP; s++) {
    uint h1 = hsh(p.x * 9781u + p.y * 6271u + s * 26699u);
    uint h2 = hsh(h1 + 0x9e3779b9u);
    float u = (h1 & 0xffff) / 65535.0, v = (h2 & 0xffff) / 65535.0;
    float rr = sqrt(u), ph = 6.2831853 * v;
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = normalize(float3(rr * cos(ph), -sqrt(max(0.0, 1.0 - u)) - 0.05, rr * sin(ph)));
    ray.TMin = 0.01;
    ray.TMax = tmax;
    P pl;
    pl.hit = 1;
    TraceRay(AS, (FL & 0xffff) | RAY_FLAG_FORCE_OPAQUE, 0xff, 0, 1, 0, ray, pl);
    hits += pl.hit;
  }
  uint t = WaveActiveSum(hits);
  if (WaveIsFirstLane()) InterlockedAdd(Out[0], t);
}
[shader("closesthit")] void ch(inout P p, in BuiltInTriangleIntersectionAttributes a) { p.hit = 1; }
[shader("miss")] void ms(inout P p) { p.hit = 0; }
)";

// An rg/ch/ms library as a pipeline (hit group "hg", payload 4 bytes, recursion 1).
ID3D12StateObject* make_so(ID3D12Device5* dev, ID3D12RootSignature* rs, const std::vector<uint8_t>& lib, HRESULT* phr) {
  D3D12_EXPORT_DESC ex[3] = {{L"rg", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"ch", nullptr, D3D12_EXPORT_FLAG_NONE},
                             {L"ms", nullptr, D3D12_EXPORT_FLAG_NONE}};
  D3D12_DXIL_LIBRARY_DESC ld{};
  ld.DXILLibrary.pShaderBytecode = lib.data();
  ld.DXILLibrary.BytecodeLength = lib.size();
  ld.NumExports = 3;
  ld.pExports = ex;
  D3D12_HIT_GROUP_DESC hg{};
  hg.HitGroupExport = L"hg";
  hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hg.ClosestHitShaderImport = L"ch";
  D3D12_RAYTRACING_SHADER_CONFIG sc{4, 8};
  D3D12_RAYTRACING_PIPELINE_CONFIG pc{1};
  D3D12_GLOBAL_ROOT_SIGNATURE grs{rs};
  D3D12_STATE_SUBOBJECT sub[5] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld},
                                  {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg},
                                  {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc},
                                  {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc},
                                  {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs}};
  D3D12_STATE_OBJECT_DESC sd{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, sub};
  ID3D12StateObject* so = nullptr;
  HRESULT hr = dev->CreateStateObject(&sd, __uuidof(ID3D12StateObject), (void**)&so);
  if (phr) *phr = hr;
  return SUCCEEDED(hr) ? so : nullptr;
}

// Its shader table: raygen, miss, hit group records, 64 bytes apart.
ID3D12Resource* make_table(Gpu& g, ID3D12StateObject* so) {
  ID3D12StateObjectProperties* props = nullptr;
  so->QueryInterface(__uuidof(ID3D12StateObjectProperties), (void**)&props);
  if (!props) return nullptr;
  uint8_t table[3 * 64] = {};
  const wchar_t* names[3] = {L"rg", L"ms", L"hg"};
  bool ids = true;
  for (int i = 0; i < 3 && ids; i++) {
    void* id = props->GetShaderIdentifier(names[i]);
    if (!id) ids = false;
    else memcpy(table + i * 64, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
  }
  props->Release();
  return ids ? g.upload(table, sizeof table) : nullptr;
}

void dispatch_rays(Gpu& g, ID3D12StateObject* so, ID3D12Resource* table, UINT w, UINT h) {
  D3D12_GPU_VIRTUAL_ADDRESS base = table->GetGPUVirtualAddress();
  D3D12_DISPATCH_RAYS_DESC dr{};
  dr.RayGenerationShaderRecord = {base, 64};
  dr.MissShaderTable = {base + 64, 64, 64};
  dr.HitGroupTable = {base + 128, 64, 64};
  dr.Width = w;
  dr.Height = h;
  dr.Depth = 1;
  g.l->SetPipelineState1(so);
  g.l->DispatchRays(&dr);
}

// vkd3d-proton's ray-query test shader (DXIL, third_party/vkd3d-proton-tests): t0 TLAS, t1 ray
// origins (float2, z = 1, direction -z), u0 float2 results, b0 {float2 miss colour, uint flags}.
// A committed hit returns (hit group index + 1, + 2); a miss the miss colour.
ID3D12RootSignature* make_rq_rs(ID3D12Device* dev) {
  auto ser = reinterpret_cast<SerializeRS_t>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!ser) return nullptr;
  D3D12_ROOT_PARAMETER p[4]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[1].Descriptor.ShaderRegister = 1;
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[3].Constants.Num32BitValues = 3;
  D3D12_ROOT_SIGNATURE_DESC d{};
  d.NumParameters = 4;
  d.pParameters = p;
  ID3DBlob *blob = nullptr, *e = nullptr;
  ID3D12RootSignature* rs = nullptr;
  if (SUCCEEDED(ser(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &e)))
    dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                             (void**)&rs);
  rel(blob);
  rel(e);
  return rs;
}

struct As {
  ID3D12Resource* buf = nullptr;
  UINT64 size = 0, scratch = 0, update_scratch = 0;
};

As build_blas(Gpu& g, const D3D12_RAYTRACING_GEOMETRY_DESC& geo, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS fl,
              ID3D12Resource** scratch_out = nullptr) {
  As r;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
  in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  in.Flags = fl;
  in.NumDescs = 1;
  in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  in.pGeometryDescs = &geo;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
  g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
  r.size = pi.ResultDataMaxSizeInBytes;
  r.scratch = pi.ScratchDataSizeInBytes;
  r.update_scratch = pi.UpdateScratchDataSizeInBytes;
  r.buf = g.buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  UINT64 sc = pi.ScratchDataSizeInBytes > pi.UpdateScratchDataSizeInBytes ? pi.ScratchDataSizeInBytes
                                                                            : pi.UpdateScratchDataSizeInBytes;
  ID3D12Resource* s = g.buffer(sc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!r.buf || !s) return r;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
  bd.Inputs = in;
  bd.DestAccelerationStructureData = r.buf->GetGPUVirtualAddress();
  bd.ScratchAccelerationStructureData = s->GetGPUVirtualAddress();
  g.l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
  g.uav_barrier(r.buf);
  if (scratch_out) *scratch_out = s;
  return r;
}

As build_tlas(Gpu& g, ID3D12Resource* inst, UINT n) {
  As r;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
  in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  in.NumDescs = n;
  in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  in.InstanceDescs = inst->GetGPUVirtualAddress();
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
  g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
  r.size = pi.ResultDataMaxSizeInBytes;
  r.scratch = pi.ScratchDataSizeInBytes;
  r.buf = g.buffer(pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
  ID3D12Resource* s = g.buffer(pi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!r.buf || !s) return r;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
  bd.Inputs = in;
  bd.DestAccelerationStructureData = r.buf->GetGPUVirtualAddress();
  bd.ScratchAccelerationStructureData = s->GetGPUVirtualAddress();
  g.l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
  g.uav_barrier(r.buf);
  return r;
}

ID3D12PipelineState* compute_pso(ID3D12Device* dev, ID3D12RootSignature* rs, const std::vector<uint8_t>& cs) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
  d.pRootSignature = rs;
  d.CS.pShaderBytecode = cs.data();
  d.CS.BytecodeLength = cs.size();
  ID3D12PipelineState* p = nullptr;
  dev->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&p);
  return p;
}

std::string run_test(ID3D12Device5* dev) {
  std::string r = "{";
  auto add = [&](const std::string& k, const std::string& v) { r += (r.size() > 1 ? "," : "") + json_str(k) + ":" + v; };
  Gpu g;
  if (!g.init(dev)) return "{\"error\":\"could not create a queue/list/fence on the game's device\"}";
  std::string err;
  // Shaders: the DXR 1.0 libraries are compiled here with the game's dxcompiler.dll (1.4.1907: no
  // inline ray queries), the ray-query test is vkd3d-proton's precompiled DXIL.
  std::vector<uint8_t> lib, rate;
  std::vector<uint8_t> rq(rayquery_code_dxil, rayquery_code_dxil + sizeof rayquery_code_dxil);
  bool c2 = dxc_compile(kLibHlsl, L"", L"lib_6_3", lib, err);
  add("compile_lib_6_3", c2 ? "true" : json_str(err));
  bool c3 = dxc_compile(kRateHlsl, L"", L"lib_6_3", rate, err);
  add("compile_rate_lib", c3 ? "true" : json_str(err));
  ID3D12RootSignature* rs = make_rs(dev, err);
  ID3D12RootSignature* rqrs = make_rq_rs(dev);
  if (!rs || !rqrs) {
    add("root_signature", json_str(err));
    rel(rs);
    rel(rqrs);
    return r + "}";
  }

  // 1. One triangle, BLAS + TLAS.
  float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  ID3D12Resource* vb = g.upload(tri, sizeof tri);
  D3D12_RAYTRACING_GEOMETRY_DESC geo{};
  geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  geo.Triangles.VertexCount = 3;
  geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
  geo.Triangles.VertexBuffer.StrideInBytes = 12;
  As blas = build_blas(g, geo, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
  D3D12_RAYTRACING_INSTANCE_DESC inst{};
  inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1;
  inst.InstanceMask = 0xff;
  inst.AccelerationStructure = blas.buf ? blas.buf->GetGPUVirtualAddress() : 0;
  ID3D12Resource* ib = g.upload(&inst, sizeof inst);
  As tlas = build_tlas(g, ib, 1);
  // out: [0..3] the pipeline's 4 rays (float), [16..] the ray query's 64 float2.
  const size_t kOut = 16 * 4 + 64 * 8;
  ID3D12Resource* out = g.buffer(kOut, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* outrb = g.buffer(kOut, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  float pos[64][2];
  for (auto& p : pos) p[0] = p[1] = 5;
  pos[0][0] = pos[0][1] = 0.25f;
  ID3D12Resource* rqpos = g.upload(pos, sizeof pos);
  if (!blas.buf || !tlas.buf || !out || !outrb || !rqpos) {
    rel(rs);
    rel(rqrs);
    return r + ",\"error\":\"acceleration structure allocation failed\"}";
  }
  add("blas1_bytes", std::to_string(blas.size));
  add("tlas1_bytes", std::to_string(tlas.size));
  std::vector<float> init(kOut / 4, -9.0f);
  ID3D12Resource* initb = g.upload(init.data(), kOut);
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.l->ResourceBarrier(1, &b);
    g.l->CopyBufferRegion(out, 0, initb, 0, kOut);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g.l->ResourceBarrier(1, &b);
  }
  // 2. Inline ray query (DXR 1.1).
  ID3D12PipelineState* pso = compute_pso(dev, rqrs, rq);
  add("rayquery_pso", pso ? "true" : "false");
  if (pso) {
    struct {
      float miss[2];
      uint32_t flags;
    } c{{1000, 2000}, 0};
    g.l->SetComputeRootSignature(rqrs);
    g.l->SetPipelineState(pso);
    g.l->SetComputeRootShaderResourceView(0, tlas.buf->GetGPUVirtualAddress());
    g.l->SetComputeRootShaderResourceView(1, rqpos->GetGPUVirtualAddress());
    g.l->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress() + 64);
    g.l->SetComputeRoot32BitConstants(3, 3, &c, 0);
    g.l->Dispatch(1, 1, 1);
    g.uav_barrier(out);
  }
  // 3. DXR 1.0 pipeline: state object + shader table + DispatchRays.
  ID3D12StateObject* so = nullptr;
  if (c2) {
    HRESULT hr = S_OK;
    so = make_so(dev, rs, lib, &hr);
    add("state_object", so ? "true" : json_str(fmt("hr %#lx", (unsigned long)hr)));
  }
  ID3D12Resource* st = so ? make_table(g, so) : nullptr;
  add("shader_table", st ? "true" : "false");
  if (st) {
    g.l->SetComputeRootSignature(rs);
    g.l->SetComputeRootShaderResourceView(0, tlas.buf->GetGPUVirtualAddress());
    g.l->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    dispatch_rays(g, so, st, 4, 1);
    g.uav_barrier(out);
  }
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.l->ResourceBarrier(1, &b);
    g.l->CopyBufferRegion(outrb, 0, out, 0, kOut);
  }
  bool ran = g.run();
  add("executed", ran ? "true" : "false");
  std::vector<float> res;
  if (ran && g.read(outrb, res, kOut / 4)) {
    add("pipeline_results", fmt("[%g,%g,%g,%g]", res[4], res[5], res[6], res[7]));
    add("pipeline_ok", (res[4] == 1 && res[5] == -1 && res[6] == -1 && res[7] == -1) ? "true" : "false");
    add("rayquery_results", fmt("[[%g,%g],[%g,%g],[%g,%g]]", res[16], res[17], res[18], res[19], res[16 + 126],
                                res[16 + 127]));
    add("rayquery_ok",
        (res[16] == 1 && res[17] == 2 && res[18] == 1000 && res[19] == 2000 && res[142] == 1000) ? "true" : "false");
  }
  rel(pso);
  rel(so);

  // 4. Sizes and timings at scale: a 256x128 bumpy grid (65536 triangles, indexed, 16-bit indices
  // like the game's meshes would be split), built fast-trace + compaction + update, and a field of
  // 1024 instances of it; then rays.
  const int GX = 257, GZ = 129;
  std::vector<float> vtx(size_t(GX) * GZ * 3);
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  for (int z = 0; z < GZ; z++)
    for (int x = 0; x < GX; x++) {
      float* v = &vtx[(size_t(z) * GX + x) * 3];
      v[0] = x * (10.0f / 256);
      v[1] = rnd() * 0.6f + 0.2f * std::sin(x * 0.3f) * std::cos(z * 0.4f);
      v[2] = z * (10.0f / 128);
    }
  std::vector<uint32_t> idx;
  idx.reserve(256 * 128 * 6);
  for (int z = 0; z < 128; z++)
    for (int x = 0; x < 256; x++) {
      uint32_t i0 = z * GX + x, i1 = i0 + 1, i2 = i0 + GX, i3 = i2 + 1;
      idx.insert(idx.end(), {i0, i2, i1, i1, i2, i3});
    }
  ID3D12Resource* gvb = g.upload(vtx.data(), vtx.size() * 4);
  ID3D12Resource* gib = g.upload(idx.data(), idx.size() * 4);
  D3D12_RAYTRACING_GEOMETRY_DESC gg{};
  gg.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
  gg.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  gg.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  gg.Triangles.VertexCount = UINT(GX * GZ);
  gg.Triangles.VertexBuffer.StartAddress = gvb->GetGPUVirtualAddress();
  gg.Triangles.VertexBuffer.StrideInBytes = 12;
  gg.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
  gg.Triangles.IndexCount = UINT(idx.size());
  gg.Triangles.IndexBuffer = gib->GetGPUVirtualAddress();
  const unsigned tris = unsigned(idx.size() / 3);
  add("grid_triangles", std::to_string(tris));
  // Static: fast trace + compaction. Dynamic (skinned-style): fast build + update.
  ID3D12Resource* post = g.buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* postrb = g.buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  g.ts(0);
  As gstat = build_blas(g, gg,
                        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION);
  g.ts(1);
  ID3D12Resource* dscratch = nullptr;
  As gdyn = build_blas(g, gg,
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
                       &dscratch);
  g.ts(2);
  if (gdyn.buf && dscratch) {  // refit in place
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    bd.Inputs.NumDescs = 1;
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.pGeometryDescs = &gg;
    bd.DestAccelerationStructureData = gdyn.buf->GetGPUVirtualAddress();
    bd.SourceAccelerationStructureData = gdyn.buf->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = dscratch->GetGPUVirtualAddress();
    g.l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    g.uav_barrier(gdyn.buf);
  }
  g.ts(3);
  if (gstat.buf) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
    pd.DestBuffer = post->GetGPUVirtualAddress();
    pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    D3D12_GPU_VIRTUAL_ADDRESS src = gstat.buf->GetGPUVirtualAddress();
    g.l->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, 1, &src);
  }
  // Instances: 32 x 32 sheets over 320 x 320 m, random yaw and lift.
  const int NI = 1024;
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC> insts(NI);
  for (int i = 0; i < NI; i++) {
    auto& d = insts[i];
    memset(&d, 0, sizeof d);
    float a = rnd() * 6.2831853f, c = std::cos(a), s = std::sin(a);
    d.Transform[0][0] = c;
    d.Transform[0][2] = s;
    d.Transform[1][1] = 1;
    d.Transform[2][0] = -s;
    d.Transform[2][2] = c;
    d.Transform[0][3] = (i % 32) * 10.0f + 5;
    d.Transform[1][3] = rnd() * 0.5f;
    d.Transform[2][3] = (i / 32) * 10.0f + 5;
    d.InstanceMask = 0xff;
    d.InstanceID = unsigned(i);
    d.AccelerationStructure = gstat.buf ? gstat.buf->GetGPUVirtualAddress() : 0;
  }
  ID3D12Resource* gin = g.upload(insts.data(), insts.size() * sizeof(insts[0]));
  g.ts(4);
  As gtlas = build_tlas(g, gin, NI);
  g.ts(5);
  {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = post;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.l->ResourceBarrier(1, &b);
    g.l->CopyBufferRegion(postrb, 0, post, 0, 8);
  }
  bool ran2 = g.run();
  if (ran2 && gstat.buf) {
    std::vector<UINT64> cs;
    g.read(postrb, cs, 1);
    add("grid_blas", fmt("{\"fast_trace_bytes\":%llu,\"compacted_bytes\":%llu,\"fast_build_update_bytes\":%llu,"
                         "\"bytes_per_tri_compacted\":%.1f,\"bytes_per_tri_fast_trace\":%.1f,\"scratch_bytes\":%llu,"
                         "\"build_fast_trace_ms\":%.3f,\"build_fast_build_ms\":%.3f,\"refit_ms\":%.3f}",
                         (unsigned long long)gstat.size, (unsigned long long)cs[0], (unsigned long long)gdyn.size,
                         double(cs[0]) / tris, double(gstat.size) / tris, (unsigned long long)gstat.scratch,
                         g.ms(0, 1), g.ms(1, 2), g.ms(2, 3)));
    add("field_tlas", fmt("{\"instances\":%d,\"bytes\":%llu,\"build_ms\":%.3f}", NI, (unsigned long long)gtlas.size,
                          g.ms(4, 5)));
  }
  // 5. Ray throughput, 1920x1080, 1..4 rays/pixel, short AO-style (3 m, any hit ends) and long rays.
  ID3D12StateObject* rso = (c3 && gtlas.buf) ? make_so(dev, rs, rate, nullptr) : nullptr;
  ID3D12Resource* rst = rso ? make_table(g, rso) : nullptr;
  add("rate_pipeline", rst ? "true" : "false");
  if (rst && ran2) {
    ID3D12Resource* cnt = g.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    struct V {
      const char* name;
      UINT spp, flags;
    } vars[] = {{"ao_3m_first_hit_1spp", 1, 4 | 8},  // ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_CLOSEST_HIT
                {"ao_3m_first_hit_4spp", 4, 4 | 8},
                {"shadow_long_first_hit_1spp", 1, 4 | 8 | 0x10000},
                {"closest_hit_long_1spp", 1, 0x10000}};
    std::string rr = "[";
    for (int rep = 0; rep < 2; rep++) {  // first pass warms up; report the second
      UINT t = 0;
      for (auto& v : vars) {
        UINT c[4] = {1920, 1080, v.spp, v.flags};
        g.l->SetComputeRootSignature(rs);
        g.l->SetComputeRootShaderResourceView(0, gtlas.buf->GetGPUVirtualAddress());
        g.l->SetComputeRootUnorderedAccessView(1, cnt->GetGPUVirtualAddress());
        g.l->SetComputeRoot32BitConstants(2, 4, c, 0);
        g.ts(10 + t);
        dispatch_rays(g, rso, rst, 1920, 1080);
        g.ts(11 + t);
        g.uav_barrier(cnt);
        t += 2;
      }
      if (!g.run()) break;
      if (rep == 1) {
        t = 0;
        for (auto& v : vars) {
          double ms = g.ms(10 + t, 11 + t);
          double rays = 1920.0 * 1080.0 * v.spp;
          rr += fmt("%s{\"variant\":\"%s\",\"ms\":%.3f,\"grays_per_s\":%.2f}", t ? "," : "", v.name, ms,
                    ms > 0 ? rays / (ms * 1e6) : 0);
          t += 2;
        }
      }
    }
    add("ray_rate", rr + "]");
    add("ray_rate_scene", fmt("\"%d instances x %u triangles (%.1f M instanced), camera 2 m above, cosine-down rays\"",
                              NI, tris, NI * double(tris) / 1e6));
  }
  rel(rso);
  rel(rs);
  rel(rqrs);
  return r + "}";
}

}  // namespace

std::string dxr_caps(ID3D12Device* dev0) {
  std::string r = "{";
  auto add = [&](const std::string& k, const std::string& v) { r += (r.size() > 1 ? "," : "") + json_str(k) + ":" + v; };
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
  HRESULT h5 = dev0->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
  add("options5", SUCCEEDED(h5) ? "true" : "false");
  if (SUCCEEDED(h5)) {
    const char* t = o5.RaytracingTier == 12                          ? "1.2"
                    : o5.RaytracingTier == D3D12_RAYTRACING_TIER_1_1 ? "1.1"
                    : o5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0"
                    : o5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED
                        ? "not supported"
                        : "other";
    add("raytracing_tier", json_str(fmt("%s (%d)", t, int(o5.RaytracingTier))));
    add("render_passes_tier", std::to_string(int(o5.RenderPassesTier)));
  }
  D3D12_FEATURE_DATA_SHADER_MODEL sm{};
  const D3D_SHADER_MODEL try_sm[] = {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
                                     D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_0};
  for (auto m : try_sm) {
    sm.HighestShaderModel = m;
    if (SUCCEEDED(dev0->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) break;
  }
  add("highest_shader_model", json_str(sm_name(sm.HighestShaderModel)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
  if (SUCCEEDED(dev0->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
    add("resource_binding_tier", std::to_string(int(o.ResourceBindingTier)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
  if (SUCCEEDED(dev0->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7))) {
    add("mesh_shader_tier", std::to_string(int(o7.MeshShaderTier)));
    add("sampler_feedback_tier", std::to_string(int(o7.SamplerFeedbackTier)));
  }
  ID3D12Device5* d5 = nullptr;
  add("device5", SUCCEEDED(dev0->QueryInterface(__uuidof(ID3D12Device5), (void**)&d5)) && d5 ? "true" : "false");
  rel(d5);
  // The adapter behind the device: name and VRAM budget (DXVK's dxgi / VK_EXT_memory_budget).
  LUID luid = dev0->GetAdapterLuid();
  IDXGIFactory4* f = nullptr;
  auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"),
                                                                                 "CreateDXGIFactory1"));
  if (create && SUCCEEDED(create(__uuidof(IDXGIFactory4), (void**)&f)) && f) {
    IDXGIAdapter3* ad = nullptr;
    if (SUCCEEDED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3), (void**)&ad)) && ad) {
      DXGI_ADAPTER_DESC2 d{};
      ad->GetDesc2(&d);
      char name[128];
      WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
      add("adapter", json_str(name));
      add("dedicated_vram_mb", std::to_string(d.DedicatedVideoMemory >> 20));
      DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
      if (SUCCEEDED(ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
        add("vram_budget_mb", std::to_string(mi.Budget >> 20));
        add("vram_usage_mb", std::to_string(mi.CurrentUsage >> 20));
      }
      ad->Release();
    }
    f->Release();
  }
  const char* vc = getenv("VKD3D_CONFIG");
  add("VKD3D_CONFIG", json_str(vc ? vc : ""));
  return r + "}";
}

std::string dxr_test(ID3D12Device* dev0) {
  ID3D12Device5* d5 = nullptr;
  if (FAILED(dev0->QueryInterface(__uuidof(ID3D12Device5), (void**)&d5)) || !d5)
    return "{\"error\":\"no ID3D12Device5\"}";
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
  dev0->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
  std::string r;
  if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) r = "{\"error\":\"raytracing tier not supported\"}";
  else r = run_test(d5);
  d5->Release();
  TB_LOG("rt dxr test: %s", r.c_str());
  return r;
}

}  // namespace tb::rt
