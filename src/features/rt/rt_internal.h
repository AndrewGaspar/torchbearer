#pragma once
// Raytracing feasibility probe, shared between its files (docs/raytracing.md). Dev only.
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12CommandQueue;
struct ID3D12CommandList;

namespace tb::rt {

// dxr.cpp
std::string dxr_caps(ID3D12Device* dev);  // CheckFeatureSupport + adapter VRAM, JSON
std::string dxr_test(ID3D12Device* dev);  // BLAS/TLAS, ray query, RT pipeline, sizes and rates, JSON

// census.cpp: D3D12 capture. Device-level hooks (resources, views, pipeline states) go in when the
// device is created (dev.rt_capture = 1 at launch) or, later, from the swapchain's device; command
// list / queue hooks on first use. Every hook is a pass-through unless a census is running.
// Hook D3D12CreateDevice (call from start); full: census too; spot: [dev] rt_spot (census.cpp).
void capture_early(bool full, int spot = 0);
void capture_device(ID3D12Device* dev);    // install the device hooks (idempotent)
bool capture_lists();                      // install command-list / queue hooks (idempotent)
bool capture_on();                         // device hooks present
std::string capture_status();

// A census of the next `frames` presented frames into `path` (text, one event per line).
bool census_begin(int frames, const std::string& path);
bool census_active();
bool census_done();
void census_present();                     // frame boundary (present listener)
void census_mark(const char* name, int phase, const void* arg);  // engine pass markers
// Engine RT object -> the ID3D12Resource inside it (pointer scan against known resources).
ID3D12Resource* engine_resource(const void* rt_object);
void census_name(const char* name, ID3D12Resource* r);

// The command list this thread is recording (last Reset / recorded call), or nullptr.
ID3D12GraphicsCommandList* cur_list();
// Last state the engine's barriers left a resource in (initial state if none seen).
unsigned res_state(ID3D12Resource* r);

// Dump captured shader blobs (and their DXIL disassembly) for shaders listed in `which` (hashes,
// comma-separated; "all" for every one) into `dir`. Returns a summary.
std::string dump_shaders(const std::string& dir, const std::string& which, bool disasm);

// ---- scene capture (census.cpp) for the ray-traced passes (scene.cpp, trace.cpp) ---------------
// With scene capture on, every DrawIndexedInstanced on a G-buffer or shadow-map pipeline whose
// shaders have been reflected (VS: g_mLocalToView / g_mViewToWorld offsets in $Globals; PS:
// discard) is recorded on the list that records it; the records move to the scene when the list
// is executed (ExecuteCommandLists), in submission order. docs/raytracing.md "Geometry capture".
enum DrawClass : uint8_t { DC_NONE = 0, DC_GBUFFER = 1, DC_SHADOW = 2, DC_SUN = 3 };
enum DrawFlags : uint8_t { DF_ALPHA = 1, DF_A2C = 2, DF_WIND = 4, DF_SKINNED = 8 };
struct DrawRec {
  uint8_t cls = 0, flags = 0;
  uint8_t ifmt = 0;      // DXGI index format (57 R16_UINT, 42 R32_UINT)
  uint8_t vfmt = 0;      // DXGI position format (6 R32G32B32_FLOAT, 2 R32G32B32A32_FLOAT)
  uint32_t stride = 0;   // vertex stride
  uint64_t vb_view = 0;  // the vertex buffer view's VA (resource lookups, invalidation)
  uint64_t vb = 0;       // VA of vertex BaseVertexLocation's position
  uint32_t vcount = 0;   // vertices from vb to the end of the view
  uint64_t ib = 0;       // VA of index StartIndexLocation
  uint32_t icount = 0;
  uint16_t l2v = 0;      // offset of g_mLocalToView in the VS's $Globals (DC_SUN: g_mShadowProjection
                         // in the PS's)
  uint64_t cb = 0;       // VA of the VS's $Globals (root CBV)
  ID3D12Resource* rt0 = nullptr;  // G-buffer: render targets 0 (device depth) and 1 (normals)
  ID3D12Resource* rt1 = nullptr;
  uint64_t ps = 0;       // pixel shader hash
  // Alpha-tested draws (DF_ALPHA; shaders/alpha.hlsli, rays.hlsl cs_gather): how the draw's own
  // shaders compute the alpha sample's UVs, and which texture it samples. uv_mode 0 = not
  // resolved (the geometry stays opaque for rays); alpha_idx < 0 likewise.
  uint8_t uv_mode = 0;        // 1 UV = A.xy * scale, 2 UV = A.xy, 3 atlas (grass cards)
  uint8_t uv_fmt_a = 0, uv_fmt_b = 0, uv_comp_a = 0;
  uint16_t uv_off_a = 0, uv_off_b = 0;  // attribute byte offsets in the UV stream's vertex
  uint64_t uv_vb = 0;         // VA of BaseVertexLocation's vertex in the UV stream's buffer view
  uint32_t uv_stride = 0;
  float uv_scale = 1.0f;      // mode 1
  uint32_t atlas_off = 0;     // mode 3: byte offset of the atlas table in $Globals
  uint8_t atlas_rows = 0;     // mode 3: float4 rows per atlas entry
  float alpha_thresh = 0.5f;
  int32_t alpha_idx = -1;     // the alpha SRV's index in the game's bound descriptor heap
  // The alpha texture itself, resolved at record time (the game rewrites its descriptors per
  // draw, so a CPU handle copied later would name another draw's texture; live, r60).
  ID3D12Resource* alpha_res = nullptr;
  uint16_t alpha_fmt = 0;     // its SRV format and mip range, as the game created the view
  uint16_t alpha_mip0 = 0, alpha_mips = 0;
  uint64_t alpha_heap = 0;    // that heap's GPU start (the spotlight binds it as space 10)
};
// Arm or disarm the capture (needs the device hooks from launch and the list hooks).
bool scene_capture(bool on);
bool scene_capture_on();
void spot_enable(bool on);  // [dev] rt_spot_on: the spotlight swap live on/off
// CPU pointer to `n` bytes at a GPU VA in an upload-heap buffer (mapped on first use), or nullptr.
const void* cpu_at(uint64_t va, size_t n);
// The buffer holding a GPU VA: resource, its VA and size, heap type, flags. False if unknown.
bool buf_at(uint64_t va, ID3D12Resource** res, uint64_t* base, uint64_t* size, int* heap, unsigned* flags);
// A resource's state at the end of the last list submitted that transitioned it (GPU order), or
// its creation state.
unsigned gpu_state(ID3D12Resource* r);
// The real ExecuteCommandLists (submits our own lists without re-entering the hook).
void exec_lists(ID3D12CommandQueue* q, unsigned n, ID3D12CommandList* const* ls);
std::string scene_capture_status();

// scene.cpp, called by census.cpp: lists just executed and the draws they recorded; a buffer range
// that was (re)created or written by a copy (cached acceleration structures over it are stale).
void scene_after_exec(ID3D12CommandQueue* q, unsigned n, ID3D12CommandList* const* ls, std::vector<DrawRec>& draws);
void scene_invalidate(uint64_t va, uint64_t size);
// scene.cpp, called by rt.cpp's engine markers (game thread).
void scene_mark(int pass, int phase, void* obj);
// The flashlight ([dev] rt_spot = 2 / flashlight_shadows = rt): whether this frame has a TLAS,
// and the root arguments (TLAS VA, constants VA, alpha buffer VA) for a swapped spotlight draw.
bool scene_spot_ready();
bool scene_spot_bind(uint64_t* tlas, uint64_t* cb, uint64_t* alpha);
void scene_spot_config(const std::string& id, const std::string& value);  // [dev] rt_spot_* tuning

// rt.cpp: an engine pool render target by name (pShadowRT, pLinearDepthRT, ...), or nullptr.
ID3D12Resource* pool_resource(const char* name);

// Output directory for probe files: %QUIVER_LAB_RUN_DIR% or the exe folder (no trailing slash).
std::string out_dir();

}  // namespace tb::rt
