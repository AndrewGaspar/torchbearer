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

// sbs_path = view (both eyes inside one engine frame): copy the back buffer's first eye-width
// columns into eye slot `eye` in the middle of the engine's frame (right after the first eye's
// renderFrame), and later copy that slot into the back buffer's second half (full SBS only).
// The back buffer is transitioned from and back to its current state (cmdlist_bb_state). Queued.
bool gpu_stash_mid(IDXGISwapChain* sc, int eye);
bool gpu_put_mid(IDXGISwapChain* sc, int eye);
// At Present in the view path: nothing to do for full SBS (composed in the frame); for half output
// the centre crops of the stashed eye and the presented frame (the other eye).
bool gpu_compose_view(IDXGISwapChain* sc, int stashed_eye);
uint32_t gpu_eye_width();  // width of one stored eye (0 before the first store)

// Counters for the status verb.
std::string gpu_status();

}  // namespace tb::stereo
