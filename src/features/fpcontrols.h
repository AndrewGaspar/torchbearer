#pragma once
// First-person control scheme (docs/first-person.md section 12): `[camera] controls = deliberate`
// (the default in first person) or `game`. Internal to the first-person feature.
#include <cstdint>

namespace tb {

// From firstperson.cpp.
bool fp_active_now();          // first person is applied to the main view (mode on, gameplay)
void* fp_player_character();   // the player's Character*, or null
bool fp_hide_all();            // hide = all (the weapon mesh is ours to hide anyway)
bool fp_sig_ok(uintptr_t rva, const char* hex);

// From fpcontrols.cpp.
void fpcontrols_register();  // config listener, lab verb `fpc`
void fpcontrols_start();     // signatures and hooks
// The gun's draw state for the viewmodel: 1 = drawn (posed in the hand), 0 = holstered (the right
// arm lowered out of view, the mesh hidden); in between while drawing / holstering. 1 whenever the
// scheme is not in use (third person, controls = game, no weapon).
double fpc_draw();
bool fpc_in_use();  // controls = deliberate is in effect (first person active, hooks in)

}  // namespace tb
