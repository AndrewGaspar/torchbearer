#pragma once

namespace tb {

// First-person camera (docs/first-person.md). `[camera] mode = first_person` moves the rendered
// camera to the player's eyes during normal gameplay (player camera, on foot) and hides the
// player's body; anything else renders the game's own camera. Inert with mode = third_person.
// Rides on the stereo feature's camera hooks through core/camera.h (filter before the eye offset).
// Lab: `quiver lab call <i> fp [status|slots N]`.
void firstperson_register();  // config listener, camera filter, lab verb (features_register)
void firstperson_start();     // signatures, the hide-mask hook (features_start)

}  // namespace tb
