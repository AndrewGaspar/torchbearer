#pragma once
// D3D12 command-list layer for stereo (docs/stereo.md section 3, "both eyes in one frame"):
//
//  - knows the swapchain's back buffers and the render-target views the engine creates on them
//    (ID3D12Device::CreateRenderTargetView, hooked when the swapchain is created);
//  - an optional trace of command-list calls that touch the back buffer (census);
//  - the overlay duplication: while armed (after the last eye's renderFrame, until Present), a
//    draw recorded on the game thread into a list whose render target is the back buffer is
//    issued twice, the second time with its viewports and scissors shifted right by one eye
//    width, so the HUD/overlays drawn once per update land in both halves of a full-SBS buffer.
//
// The ID3D12GraphicsCommandList vtable is only patched once something asks for it
// (cmdlist_install); until then, and whenever neither the trace nor the duplication is armed,
// every hook is a pass-through.
#include <cstdint>
#include <string>

struct IDXGISwapChain;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace tb::stereo {

// From the swapchain-created/resized listener: remember the back buffers, hook RTV creation.
void cmdlist_on_swapchain(IDXGISwapChain* sc);
// Patch the command-list vtable (idempotent). Needs the swapchain's queue. False on failure.
bool cmdlist_install(IDXGISwapChain* sc);
bool cmdlist_installed();

// Trace command-list calls touching back buffers (and draw counts per list) into the log.
void cmdlist_trace(bool on);

// Track per-list render targets / viewports (needed before cmdlist_duplicate can work: a list's
// render target may be bound before the overlay phase starts).
void cmdlist_track(bool on);

// Overlay duplication: draws into the back buffer on thread `tid` are repeated shifted by `dx`
// pixels. dx = 0 disarms.
void cmdlist_duplicate(unsigned long tid, uint32_t dx);

// The engine's command list (still open) that last bound a back buffer as render target, or
// nullptr. Commands recorded into it run in the engine's own order (tracking must be on).
ID3D12GraphicsCommandList* cmdlist_bb_list();

// The state the engine's command lists last left back buffer `bb` in (from its ResourceBarrier
// calls; D3D12_RESOURCE_STATE_PRESENT = 0 if none seen). Valid once the vtable is installed.
unsigned cmdlist_bb_state(ID3D12Resource* bb);

std::string cmdlist_status();

}  // namespace tb::stereo
