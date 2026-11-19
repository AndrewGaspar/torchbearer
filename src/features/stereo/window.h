#pragma once
#include <string>

namespace tb::stereo {

// Full side-by-side output needs a window twice as wide as the engine renders (3840x1080 for
// 1920x1080 per eye). The real window is widened while the engine keeps seeing one eye's size:
// WM_SIZE and mouse positions are translated in the game's window procedure. Active only when
// the swapchain width scale is 2 ([stereo] output = full). docs/stereo.md, "Window".
void window_start();
std::string window_status();

}  // namespace tb::stereo
