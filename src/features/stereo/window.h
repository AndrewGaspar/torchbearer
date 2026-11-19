#pragma once
#include <string>

namespace tb::stereo {

// Full side-by-side output needs a window twice as wide as the engine renders (3840x1080 for
// 1920x1080 per eye). The real window is widened while the engine keeps seeing one eye's size:
// WM_SIZE and mouse positions are translated in the game's window procedure. Active only when
// the swapchain width scale is 2 ([stereo] output = full). docs/stereo.md, "Window".
void window_start();
// How a cursor position over the window maps to the engine's (one eye's) coordinates:
//   stretched = false (output = full): each eye occupies its own half of the window, 1:1, so the
//               game sees the position within the half the cursor is over (x mod eye width);
//   stretched = true  (output = hsbs): the compositor (HypXRland) shows each eye's half of the
//               window stretched over the whole window in that eye, so the cursor at x is at x/2
//               in the eye, in both eyes.
void window_set_stretched(bool stretched);
std::string window_status();

}  // namespace tb::stereo
