#pragma once
#include <cstdint>
#include <functional>

struct IDXGISwapChain;
struct IDXGIFactory;

namespace tb {

// DXGI factory/swapchain hooks. Listeners run on the game's presenting thread, just before the
// real Present (and after ResizeBuffers).
void present_hook_factory(IDXGIFactory* factory);

using PresentFn = std::function<void(IDXGISwapChain* sc, uint64_t frame)>;
void present_on(PresentFn fn);
using ResizeFn = std::function<void(IDXGISwapChain* sc, unsigned w, unsigned h)>;
void resize_on(ResizeFn fn);

uint64_t present_frame();

// Present filters run after the listeners; if any returns false the real Present is skipped
// (the call returns S_OK and the back buffer is not flipped). Used by stereo to drop the first
// of two engine frames per game update.
using PresentFilterFn = std::function<bool(IDXGISwapChain* sc, uint64_t frame)>;
void present_filter(PresentFilterFn fn);

// Swapchain width multiplier (1 or 2), applied when the game creates or resizes its swapchain:
// with 2 the real buffers are twice as wide as the game asked for (full side-by-side output),
// while GetDesc/GetDesc1 keep reporting the game's own width. Set it before the swapchain exists.
void present_set_width_scale(unsigned scale);
unsigned present_width_scale(IDXGISwapChain* sc);  // the scale this swapchain was created with
unsigned present_width_scale_pending();             // the scale new swapchains will get

// The object the swapchain was created with: for Direct3D 12 that is the game's
// ID3D12CommandQueue (not AddRef'd; lives as long as the swapchain). nullptr if unknown.
void* present_queue(IDXGISwapChain* sc);

}  // namespace tb
