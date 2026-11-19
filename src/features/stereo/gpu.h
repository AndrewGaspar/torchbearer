#pragma once
// D3D12 helpers on the game's own command queue (the one its swapchain was created with):
// back-buffer readback for measurements, and the eye copies of the side-by-side compositor.
#include <cstdint>
#include <string>

struct IDXGISwapChain;
struct ID3D12Resource;

namespace tb::stereo {

// Save the back buffer about to be presented as a 24-bit BMP (blocks until the copy is done).
// Called from a Present listener. Returns false (and logs why) on failure.
bool gpu_capture(IDXGISwapChain* sc, const std::string& path);

// Copy the back buffer about to be presented into eye slot `eye` (0 = left, 1 = right), keeping it
// for gpu_compose. Queued on the game's queue; does not block.
bool gpu_store_eye(IDXGISwapChain* sc, int eye);

// Write the two stored eyes into the back buffer about to be presented: each squeezed into its
// half (half-SBS), or, when the back buffer is twice as wide as an eye, copied 1:1 (full SBS).
// Queued on the game's queue; does not block.
bool gpu_compose(IDXGISwapChain* sc);

// Counters for the status verb.
std::string gpu_status();

}  // namespace tb::stereo
