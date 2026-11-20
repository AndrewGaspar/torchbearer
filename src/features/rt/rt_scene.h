#pragma once
// Ray-traced passes on the engine's own frame (docs/raytracing.md "Geometry capture", "RTAO"):
// scene.cpp captures the G-buffer and shadow-map geometry, keeps one BLAS per mesh and builds a
// TLAS per game update on a command list of ours submitted right after the engine's mid-frame
// kick (all G-buffer and shadow lists are in it, the lighting lists come after it); trace.cpp
// records the ray passes (validation, AO, sun shadow) and their denoiser into the same list, as
// compute shaders with inline ray queries (shaders/rays.hlsl, precompiled), alpha-tested geometry
// tested against its own texture (shaders/alpha.hlsli, docs/raytracing.md section 13).
#include <cstdint>
#include <string>

struct ID3D12Device5;
struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;

namespace tb::rt {

// The engine's $Globals header, the same in every shader (offsets 0..400, static: all 240
// captured shaders): matrices are row_major float4x4 used as mul(M, v) (translation in .w of the
// first three rows; established from the G-buffer VS, which loads rows 0-2 of g_mLocalToView).
struct Camera {
  float w2v[16], v2w[16], v2c[16], c2v[16], w2c[16], c2w[16];
  float view_point[4];
};

// One eye's trace inputs for this frame.
struct EyeInputs {
  int eye = 0;               // stereo eye: 0 mono / left, 1 right
  Camera cam;                // from the eye's first G-buffer draw
  double anchor[3] = {};     // world origin of the TLAS (the update's first camera position)
  ID3D12Resource* depth = nullptr;   // G-buffer RT0, R32_FLOAT device depth (z/w)
  ID3D12Resource* normal = nullptr;  // pNormalRT, RGBA16F view-space normal * 0.5 + 0.5
  unsigned depth_state = 0, normal_state = 0;  // their states at this point (GPU order)
  uint64_t tlas = 0;         // TLAS GPU VA (0: none)
  uint64_t frame = 0;        // update counter (present frame)
  double w2c[16] = {};       // this eye's world (absolute) -> clip, doubles (history reprojection)
  uint64_t alpha = 0;        // the update's alpha buffer (records + UVs; alpha.hlsli), 0: none
  int alpha_slot = -1;       // the slot whose descriptor region holds the alpha textures
  uint64_t motion = 0;       // per TLAS instance: previous * inverse(current) transform, 3x4 floats
};

// trace.cpp
bool trace_init(ID3D12Device5* dev, std::string& err);  // shaders, pipelines, root signature
// Record this eye's passes into `l` (our list). `slot` is the frame-in-flight ring slot (0..3),
// used for upload/descriptor slices. Returns the AO output for the eye (R16G16_FLOAT) or nullptr.
ID3D12Resource* trace_record(ID3D12GraphicsCommandList4* l, const EyeInputs& in, int slot);
// GPU timestamp i (0 = the list's start) into the slot's query range.
void trace_mark(ID3D12GraphicsCommandList4* l, int slot, int i);
// The slot's descriptor regions for the alpha path: the alpha textures (kAlphaTex descriptors, the
// mod's copies of the game's) and the buffers the UV gather reads (kGatherBufs raw views).
constexpr int kAlphaTex = 8192, kGatherBufs = 1024;
bool trace_alpha_heap(int slot, uint64_t* tex_cpu, uint64_t* buf_cpu, unsigned* inc);
// Record the UV gather (cs_gather) into `l`: jobs (and groups at byte `groups_off`) at `jobs`,
// output (a UAV buffer, the alpha buffer) at `uv`.
void trace_gather(ID3D12GraphicsCommandList4* l, int slot, uint64_t jobs, uint32_t groups_off, uint32_t ngroups,
                  uint64_t uv);
void trace_timing_reset();
void trace_set_sun(const float world_dir_to_light[3]);  // from the sun resolve's constants
void trace_config(const std::string& id, const std::string& value);  // [dev] rtao_* / rt_sun_*
// Called when the slot's GPU work has completed: harvest validation stats and timings.
void trace_slot_done(int slot);
// Arm a validation pass (primary rays vs the G-buffer depth) for the next traced frame.
void trace_validate_arm(unsigned mask, bool image, bool alpha = true);
std::string trace_status();
// The AO result for an eye in the state COPY_SOURCE, if this frame produced one for it.
ID3D12Resource* trace_ao_result(int eye, uint64_t frame);
// The sun / moon visibility for an eye (B8G8R8A8, R = visibility, COPY_SOURCE), [dev] rt_sun = 1.
ID3D12Resource* trace_sun_result(int eye, uint64_t frame);

// scene.cpp
bool scene_enable(bool on);  // capture + passes on/off (needs the device hooks from launch)
bool scene_enabled();
std::string scene_status();
std::string scene_verb(const std::string& sub, const std::string& arg);

}  // namespace tb::rt
