#pragma once
// Main-camera extension points (docs/first-person.md, "Stereo integration").
//
// The stereo feature owns the engine hooks this rides on (View::setViewToWorld and
// RendererX86::renderFrame, MinHook allows one hook per function) and calls into here from them on
// the game thread, whatever its own mode is (stereo mode = off included). Features register in
// features_register(), before any hook runs; the lists are not changed afterwards.

namespace tb {

// A filter may rewrite the transform the game is setting on a view: m is a double[12] in the
// engine's row-vector layout (rows right, up, forward, position). It runs for every
// setViewToWorld call (about 15 per frame) and decides itself whether `view` is its camera.
// Return true when m was changed. Filters run in ascending `order`, all of them before the stereo
// eye offset, so the stereo offset is applied to the filtered transform.
using CameraFilter = bool (*)(void* view, double* m);
void camera_filter_add(int order, CameraFilter f);

// Called with the main scene view at the top of each renderFrame (once per game update, also in
// sbs where both eyes follow), before stereo touches the view: projection parameters (FOV, near
// plane) set here are what both eyes render with.
using CameraPreRender = void (*)(void* view);
void camera_prerender_add(CameraPreRender f);

// For the hook owner (stereo.cpp).
bool camera_filter_run(void* view, const double* in, double* out);  // true: use `out`
void camera_prerender_run(void* view);
void camera_set_hooked(bool on);
bool camera_hooked();  // the hooks above are installed (stereo signatures matched)

}  // namespace tb
