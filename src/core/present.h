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

}  // namespace tb
