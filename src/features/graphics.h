#pragma once
#include <string>

namespace tb {

// Rendering options the game's menu does not have (docs/raytracing.md, "The game's built-in SSAO").
//  - [graphics] ssao = 1: the game's original (2010) screen-space ambient occlusion instead of the
//    remaster's HBAO. The remaster ships SSAO but never enables it ("Render SSAO", u8 0xd755e0, is
//    written 0 by every graphics preset), and HBAO's apply would overwrite its result (both write
//    the shadow buffer's alpha), so the option runs SSAO and skips HBAO. The main pass's branches
//    are patched instead of the globals, so nothing the game saves to its config changes; 0
//    restores the game's own branches. Applied between frames.
//  - [graphics] ao = game, ssao or rtao: the ambient occlusion. game = the remaster's HBAO, ssao =
//    the above, rtao = ray-traced AO (rt/trace.cpp) written where HBAO's result goes. Set, it
//    takes precedence over `ssao`; unset
//    (empty), `ssao` decides. rtao needs the D3D12 capture, installed at launch only when the
//    game starts with ao = rtao.
//  - [graphics] flashlight_shadows = game or rt: ray-traced flashlight shadows (rt/scene.cpp,
//    shaders/spotlight.hlsl) instead of its shadow-map PCF. Also launch-only (the capture).
void graphics_register();
void graphics_start();

// SSAO forced on by the option (the global 0xd755e0 stays the game's).
bool graphics_ssao_on();

// Dev (rt flag): the SSAO or HBAO branch of the main pass. mode -1 the game's, 1 run, 0 skip.
// Applied at the next frame boundary. False on a signature mismatch.
bool graphics_branch(const std::string& which, int mode);
std::string graphics_branch_state(const std::string& which);

}  // namespace tb
