#pragma once
// Raytracing feasibility probe, shared between its files (docs/raytracing.md). Dev only.
#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace tb::rt {

// dxr.cpp
std::string dxr_caps(ID3D12Device* dev);  // CheckFeatureSupport + adapter VRAM, JSON
std::string dxr_test(ID3D12Device* dev);  // BLAS/TLAS, ray query, RT pipeline, sizes and rates, JSON

// census.cpp: D3D12 capture. Device-level hooks (resources, views, pipeline states) go in when the
// device is created (dev.rt_capture = 1 at launch) or, later, from the swapchain's device; command
// list / queue hooks on first use. Every hook is a pass-through unless a census is running.
void capture_early();                      // hook D3D12CreateDevice (call from start)
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

// Output directory for probe files: %QUIVER_LAB_RUN_DIR% or the exe folder (no trailing slash).
std::string out_dir();

}  // namespace tb::rt
