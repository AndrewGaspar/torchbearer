#pragma once
// Per-eye temporal state for dual rendering (docs/stereo.md, "Temporal state per eye").
//
// When the scene is rendered twice per game update (sbs) or once per eye on alternating frames,
// every piece of renderer state that carries "last frame" into this frame would otherwise carry
// the OTHER eye: TAA history, the TAA ping-pong index, the previous camera matrices behind camera
// and object motion vectors. This module keeps one copy per eye and swaps it in around each eye's
// RendererX86::renderFrame call (eyestate_enter / eyestate_leave), so it does not depend on how the
// caller structures the dual render. Everything here is inert unless eyestate_enter is called
// with an eye.
#include <string>

namespace tb::stereo {

struct EyeOpts {
  bool taa_history = true;  // per-eye TAA history RTs + ping-pong index (else shared)
  bool view_prev = true;    // per-eye previous camera matrices (camera + object motion vectors)
  bool same_jitter = true;  // both eyes of an update use the same TAA jitter, advancing per update
  // Another eye of the same game update renders after this one (sbs, first eye): keep the
  // objects' previous transforms so the next eye still sees the previous update's (with
  // view_prev).
  bool another_eye_follows = false;
};

// Signature checks and hooks (RT pool allocator, renderWorld). False: per-eye state unavailable
// (logged); the caller must then fall back to switching TAA / object velocity off.
bool eyestate_start();
bool eyestate_available();
bool eyestate_history_available();  // the per-eye TAA history part specifically

// eye: -1 left, +1 right. Call immediately before / after the eye's renderFrame, on the game
// thread. `first_of_update` marks the first eye rendered for a game update (jitter advance).
void eyestate_enter(void* renderer, void* view, int eye, bool first_of_update, const EyeOpts& o);
void eyestate_leave(void* renderer, void* view, int eye);
// Forget the saved per-eye state (mode change, level load): the next render of each eye starts
// from whatever the engine has.
void eyestate_reset();

// Once per game update: `dual` = this update renders more than one eye (sbs). With DLSS on, the
// right eye then gets its own DLSS feature (dlss.cpp; fallback: DLSS reset every frame).
void eyestate_update(bool dual);

// Debug: log the view's previous/current camera matrices around the next N eye renders.
void eyestate_probe(int n);
std::string eyestate_status_json();

}  // namespace tb::stereo
