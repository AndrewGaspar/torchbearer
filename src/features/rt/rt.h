#pragma once

namespace tb::rt {

// Raytracing feasibility probe (docs/raytracing.md). Dev only: the D3D12 capture is installed at
// launch when [dev] rt_capture = 1, everything else on demand from `quiver lab call <i> rt ...`.
// With neither, nothing is hooked.
void register_feature();  // lab verbs
void start();             // [dev] rt_capture / [graphics] ao = rtao: hook D3D12CreateDevice
// Ray-traced ambient occlusion ([graphics] ao = rtao, graphics.cpp): wanted on/off (applied at
// the next present; needs the capture installed at launch) and whether it is running.
void set_rtao(bool on);
bool rtao_active();

}  // namespace tb::rt
