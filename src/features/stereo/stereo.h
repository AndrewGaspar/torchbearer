#pragma once

namespace tb::stereo {

// Stereoscopic 3D (docs/stereo.md). Inert unless [stereo] enabled = 1 or a lab verb arms the
// census; with the feature off every hook is a pass-through.
void register_feature();  // config listeners, lab verbs (from features_register)
void start();             // signature checks and hooks (from features_start)
// The eye of the render in progress: -1 left, +1 right, 0 mono (game thread, inside renderFrame).
int eye_now();
// The rend::View last passed to renderFrame (the main camera; game thread), or nullptr.
void* main_view();

}  // namespace tb::stereo
