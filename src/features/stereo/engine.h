#pragma once
// Northlight (Alan Wake Remastered, Game_f_x64_EOS.exe) renderer entry points and the rend::View
// layout the stereo feature relies on. Every address is an RVA (image base 0x140000000), found in
// the Ghidra project (re/scripts/Serve.java) and checked live; docs/stereo.md says how each one was
// established. Each is guarded by a prologue signature (engine_check) before anything is hooked.
#include <cstddef>
#include <cstdint>

namespace tb::stereo {

// --- functions ---------------------------------------------------------------------------------
// rend::RendererX86 vtable slot 11 (+0x58): builds the TAA jitter (post-projection matrix), sets it
// on the view, then calls renderWorld. void(RendererX86*, View*, void* scene, void* extra).
constexpr uintptr_t kRvaRenderFrame = 0x508970;
// The world/scene render it calls: allocates the frame's RTs (pColorRT, pTAAHistoryRT0/1, ...),
// renders shadows, G-buffer, lighting, post. void(RendererX86*, View*, void*, void*, void*).
constexpr uintptr_t kRvaRenderWorld = 0x5090b0;
// rend::View::setViewToWorld(const double m[4][3]): stores the camera transform, inverts it to
// world-to-view, marks world-to-clip dirty, writes g_mWorldToView / g_mViewToWorld / g_vViewPoint.
constexpr uintptr_t kRvaSetViewToWorld = 0x15e2f0;
// rend::View::setPostProjection(const double m[4][4]) (+0x08), marks projection dirty. The TAA
// jitter lives in m[3][0..1].
constexpr uintptr_t kRvaSetPostProjection = 0x46c090;
// ModeSwitch::render (game thread): frame pacing, Renderer::beginFrame, each mode's render(),
// Renderer::endFrame.
constexpr uintptr_t kRvaModeSwitchRender = 0x262cb0;
// Game-level renderer (global at kRvaGameRenderer) beginFrame / endFrame.
constexpr uintptr_t kRvaBeginFrame = 0x2aca30;
constexpr uintptr_t kRvaEndFrame = 0x2acb10;

struct Sig {
  uintptr_t rva;
  const char* name;
  uint8_t bytes[16];
};
// First 16 bytes of each hooked function (from the shipped exe).
inline constexpr Sig kSigs[] = {
    {kRvaRenderFrame, "RendererX86::renderFrame",
     {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x55}},
    {kRvaRenderWorld, "RendererX86::renderWorld",
     {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x4c, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48}},
    {kRvaSetViewToWorld, "View::setViewToWorld",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x55, 0x48, 0x8b, 0xec, 0x48, 0x83}},
    {kRvaSetPostProjection, "View::setPostProjection",
     {0x48, 0x8b, 0x02, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8b, 0x42, 0x08, 0x48, 0x89, 0x41, 0x10, 0x48}},
    {kRvaModeSwitchRender, "ModeSwitch::render",
     {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81, 0xec}},
    {kRvaBeginFrame, "Renderer::beginFrame",
     {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xff, 0x05, 0x9c, 0xbe, 0x86, 0x00, 0x48, 0x8b, 0xd9, 0xc6}},
    {kRvaEndFrame, "Renderer::endFrame",
     {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81}},
};

// --- rend::View (0xcb0 bytes; PerspectiveView 0xcc8) -------------------------------------------
// All matrices are doubles, row-vector convention (v' = v * M), translation in the last row.
namespace view {
constexpr size_t kPostProj = 0x008;     // double[4][4], set by setPostProjection (TAA jitter)
constexpr size_t kWorldToView = 0x1e8;  // double[4][3], inverse of kViewToWorld
constexpr size_t kViewToWorld = 0x248;  // double[4][3], rows: right, up, forward, position
constexpr size_t kViewToClip = 0x2a8;   // double[4][4], projection * post-projection (lazy)
constexpr size_t kWorldToClip = 0x3a8;  // double[4][4] (lazy)
constexpr size_t kDirtyProj = 0x4a8;    // u8: viewToClip valid
constexpr size_t kDirtyWorld = 0x4a9;   // u8: worldToClip valid
// PerspectiveView parameters (floats). m00 = 1/tan(fov/2)/zoom, m11 = m00*aspect.
constexpr size_t kFov = 0xcb0;          // horizontal FOV, radians
constexpr size_t kZoom = 0xcb4;
constexpr size_t kAspect = 0xcb8;       // width / height
constexpr size_t kNear = 0xcbc;
constexpr size_t kFar = 0xcc0;
}  // namespace view

// RendererX86 -> TAA object (TAA::beginFrame RVA 0x498590 is called with it from renderWorld).
// TAA+0x60: previous frame's unjittered world-to-clip, double[4][4]; TAA::beginFrame derives
// current-view-to-previous-clip (camera motion vectors) from it, then overwrites it.
constexpr size_t kRendererTaa = 0x140;
constexpr size_t kTaaPrevWorldToClip = 0x60;

// --- tweakable registry (rend::BaseTweakable) ---------------------------------------------------
// Tweakable ctor RVA 0x461320(this, name, type) appends `this` to a global array of at most 0x500
// pointers (count at kRvaTweakCount) under a mutex. Most tweakables are function-local statics, so
// one exists only once its owning code has run. Layout: +0x00 char* name (r::String, inline
// buffer at +0x28), +0xa8 u32 type (0 bool, 1 float, 5 list, ...), +0xb0 value (u8 / float),
// +0xb4 min, +0xb8 max (floats).
constexpr uintptr_t kRvaTweakArray = 0xd386f0;
constexpr uintptr_t kRvaTweakCount = 0xd3aef8;
constexpr size_t kTweakType = 0xa8;
constexpr size_t kTweakValue = 0xb0;

// vtable RVAs of the View classes (to tell them apart in a census).
constexpr uintptr_t kRvaVtblPerspectiveView = 0x751bd8;
constexpr uintptr_t kRvaVtblView = 0x751bb8;

}  // namespace tb::stereo
