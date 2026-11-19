#pragma once
// Per-eye history of the screen-space effects that keep their own temporal buffer (docs/stereo.md
// section 5, "SSAO and volumetric light"): rend::SSAO (RendererX86+0x130) and
// rend::VolumetricLight (RendererX86+0x170) each blend this frame's result into ONE persistent
// "Temporal buffer" and remember the camera they last rendered with. With two eyes per update
// each eye would blend into the other eye's result, and the eye offset would read as camera
// motion (the blend weight drops to its minimum). rend::ShadeEffect (RendererX86+0x1b0, the
// Taken darkness around the birds and enemies) keeps a trailing feedback buffer per ring slot and
// a fade stepped per render: the second eye drew into the first eye's result. The left eye (and
// mono) keeps the engine's own buffers; the right eye gets its own, created through the engine's
// own device, plus its own remembered camera, reset flags and fade, swapped in around its
// renderFrame. Inert unless temporal_enter is called with eye > 0.
#include <string>

namespace tb::stereo {

void temporal_register();  // config listener (stereo.temporal_per_eye)
bool temporal_start();     // signature checks; false: per-eye history unavailable (logged)
// eye: -1 left, +1 right (0: nothing). Immediately before / after the eye's renderFrame, on the
// game thread.
void temporal_enter(void* renderer, void* view, int eye);
void temporal_leave(void* renderer, int eye);
// Forget the right eye's camera and make its next render start without history (sbs toggled).
void temporal_reset();
std::string temporal_status();

}  // namespace tb::stereo
