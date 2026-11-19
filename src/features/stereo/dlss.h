#pragma once
// One DLSS feature per eye in sbs (docs/stereo.md, section 5 "DLSS").
//
// The engine keeps one NGX DLSS feature in its D3D12 device (handle, the mode/size key it was
// created for). The left eye uses the engine's own; the right eye gets a second feature, swapped
// into the device around its renderFrame, so each eye evaluates against its own temporal history.
// When that is unavailable (signature mismatch, creation failure, stereo.dlss_per_eye = 0) the
// evaluate is told to Reset every frame instead (no cross-eye blending, no accumulation).
// Inert unless dlss_update(true) was called for the current game update.
#include <string>

namespace tb::stereo {

// Signature checks; false = per-eye DLSS unavailable (the reset fallback still works if its own
// signature matched: `reset_ok`).
bool dlss_start(bool reset_ok);
// Once per game update, on the game thread, before anything renders. `dual`: both eyes of this
// update are rendered (sbs). Releases the second feature when not dual; switches the reset
// fallback on/off.
void dlss_update(bool dual);
// Around the right eye's renderFrame (eye > 0), game thread; no-ops for the left eye.
void dlss_eye_enter(int eye);
void dlss_eye_leave(int eye);
std::string dlss_status_json();

}  // namespace tb::stereo
