#include "core/present.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"

namespace tb {
namespace {

// Vtable slots (dxgi.h declaration order).
constexpr size_t kFactoryCreateSwapChain = 10;
constexpr size_t kFactory2CreateSwapChainForHwnd = 15;
constexpr size_t kSwapPresent = 8;
constexpr size_t kSwapResizeBuffers = 13;
constexpr size_t kSwap1Present1 = 22;
constexpr size_t kSwapGetDesc = 12;
constexpr size_t kSwap1GetDesc1 = 18;

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using GetDesc_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using GetDesc1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

CreateSwapChain_t o_CreateSwapChain;
CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd;
Present_t o_Present;
Present1_t o_Present1;
ResizeBuffers_t o_ResizeBuffers;
GetDesc_t o_GetDesc;
GetDesc1_t o_GetDesc1;

std::mutex g_mu;
std::vector<PresentFn> g_present;
std::vector<ResizeFn> g_resize;
std::vector<PresentFilterFn> g_filters;
std::atomic<unsigned> g_width_scale{1};
std::vector<std::pair<IDXGISwapChain*, unsigned>> g_scaled;  // swapchains created with a scale

unsigned scale_of(IDXGISwapChain* sc) {
  std::lock_guard lk(g_mu);
  for (auto& [s, k] : g_scaled)
    if (s == sc) return k;
  return 1;
}
std::atomic<uint64_t> g_frame{0};
std::vector<std::pair<IDXGISwapChain*, IUnknown*>> g_queues;  // swapchain -> creating device/queue

void remember_queue(IDXGISwapChain* sc, IUnknown* dev) {
  std::lock_guard lk(g_mu);
  for (auto& [s, q] : g_queues)
    if (s == sc) {
      q = dev;
      return;
    }
  g_queues.emplace_back(sc, dev);
}

// Returns false if a filter vetoed the real Present.
bool on_present(IDXGISwapChain* sc) {
  uint64_t f = g_frame.fetch_add(1) + 1;
  if (f == 1) {
    TB_LOG("first present");
    lab_state("running", {"frame=1"});
  }
  std::vector<PresentFn> ls;
  {
    std::lock_guard lk(g_mu);
    ls = g_present;
  }
  for (auto& fn : ls) fn(sc, f);
  std::vector<PresentFilterFn> fs;
  {
    std::lock_guard lk(g_mu);
    fs = g_filters;
  }
  bool present = true;
  for (auto& fn : fs) present = fn(sc, f) && present;
  return present;
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST) && !on_present(sc)) return S_OK;
  return o_Present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* p) {
  if (!(flags & DXGI_PRESENT_TEST) && !on_present(sc)) return S_OK;
  return o_Present1(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt,
                                           UINT flags) {
  unsigned k = scale_of(sc);
  HRESULT hr = o_ResizeBuffers(sc, n, w ? w * k : w, h, fmt, flags);
  TB_LOG("ResizeBuffers(%u, %ux%u, fmt %d, flags 0x%x) = 0x%lx", n, w, h, int(fmt), flags, hr);
  if (SUCCEEDED(hr)) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    std::vector<ResizeFn> ls;
    {
      std::lock_guard lk(g_mu);
      ls = g_resize;
    }
    for (auto& fn : ls) fn(sc, d.BufferDesc.Width, d.BufferDesc.Height);
  }
  return hr;
}

// With a width scale the game keeps seeing the width it asked for.
HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d) {
  HRESULT hr = o_GetDesc(sc, d);
  unsigned k = scale_of(sc);
  if (SUCCEEDED(hr) && d && k > 1) d->BufferDesc.Width /= k;
  return hr;
}
HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGISwapChain1* sc, DXGI_SWAP_CHAIN_DESC1* d) {
  HRESULT hr = o_GetDesc1(sc, d);
  unsigned k = scale_of(sc);
  if (SUCCEEDED(hr) && d && k > 1) d->Width /= k;
  return hr;
}

void hook_swapchain(IDXGISwapChain* sc) {
  o_Present = reinterpret_cast<Present_t>(hook_vtable(sc, kSwapPresent, (void*)hk_Present));
  o_ResizeBuffers =
      reinterpret_cast<ResizeBuffers_t>(hook_vtable(sc, kSwapResizeBuffers, (void*)hk_ResizeBuffers));
  o_GetDesc = reinterpret_cast<GetDesc_t>(hook_vtable(sc, kSwapGetDesc, (void*)hk_GetDesc));
  IDXGISwapChain1* sc1 = nullptr;
  if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
    o_Present1 = reinterpret_cast<Present1_t>(hook_vtable(sc1, kSwap1Present1, (void*)hk_Present1));
    o_GetDesc1 = reinterpret_cast<GetDesc1_t>(hook_vtable(sc1, kSwap1GetDesc1, (void*)hk_GetDesc1));
    sc1->Release();
  }
  DXGI_SWAP_CHAIN_DESC d{};
  sc->GetDesc(&d);
  TB_LOG("swapchain %p: %ux%u fmt %d, %u buffers, effect %d, flags 0x%x, windowed %d", (void*)sc,
         d.BufferDesc.Width, d.BufferDesc.Height, int(d.BufferDesc.Format), d.BufferCount,
         int(d.SwapEffect), d.Flags, d.Windowed);
  std::vector<ResizeFn> ls;
  {
    std::lock_guard lk(g_mu);
    ls = g_resize;
  }
  for (auto& fn : ls) fn(sc, d.BufferDesc.Width, d.BufferDesc.Height);
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc,
                                             IDXGISwapChain** out) {
  unsigned k = g_width_scale.load();
  DXGI_SWAP_CHAIN_DESC d2;
  if (desc && k > 1 && desc->BufferDesc.Width) {
    d2 = *desc;
    d2.BufferDesc.Width *= k;
    desc = &d2;
  }
  HRESULT hr = o_CreateSwapChain(f, dev, desc, out);
  if (SUCCEEDED(hr) && out && *out && k > 1) {
    std::lock_guard lk(g_mu);
    g_scaled.emplace_back(*out, k);
  }
  TB_LOG("CreateSwapChain = 0x%lx", hr);
  if (SUCCEEDED(hr) && out && *out) {
    remember_queue(*out, dev);
    hook_swapchain(*out);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                    IDXGIOutput* out, IDXGISwapChain1** sc) {
  unsigned k = g_width_scale.load();
  DXGI_SWAP_CHAIN_DESC1 d2;
  if (desc && k > 1 && desc->Width) {
    d2 = *desc;
    d2.Width *= k;
    desc = &d2;
    TB_LOG("swapchain width x%u: %u -> %u", k, d2.Width / k, d2.Width);
  }
  HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, sc);
  if (SUCCEEDED(hr) && sc && *sc && k > 1) {
    std::lock_guard lk(g_mu);
    g_scaled.emplace_back(*sc, k);
  }
  TB_LOG("CreateSwapChainForHwnd(%ux%u fmt %d, %u buffers) = 0x%lx", desc ? desc->Width : 0,
         desc ? desc->Height : 0, desc ? int(desc->Format) : -1, desc ? desc->BufferCount : 0, hr);
  if (SUCCEEDED(hr) && sc && *sc) {
    remember_queue(*sc, dev);
    hook_swapchain(*sc);
  }
  return hr;
}

}  // namespace

void present_hook_factory(IDXGIFactory* factory) {
  if (!factory) return;
  o_CreateSwapChain = reinterpret_cast<CreateSwapChain_t>(
      hook_vtable(factory, kFactoryCreateSwapChain, (void*)hk_CreateSwapChain));
  IDXGIFactory2* f2 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
    o_CreateSwapChainForHwnd = reinterpret_cast<CreateSwapChainForHwnd_t>(
        hook_vtable(f2, kFactory2CreateSwapChainForHwnd, (void*)hk_CreateSwapChainForHwnd));
    f2->Release();
  }
}

void present_on(PresentFn fn) {
  std::lock_guard lk(g_mu);
  g_present.push_back(std::move(fn));
}

void resize_on(ResizeFn fn) {
  std::lock_guard lk(g_mu);
  g_resize.push_back(std::move(fn));
}

uint64_t present_frame() { return g_frame.load(); }

void present_filter(PresentFilterFn fn) {
  std::lock_guard lk(g_mu);
  g_filters.push_back(std::move(fn));
}

void present_set_width_scale(unsigned scale) { g_width_scale = scale ? scale : 1; }
unsigned present_width_scale(IDXGISwapChain* sc) { return scale_of(sc); }

void* present_queue(IDXGISwapChain* sc) {
  std::lock_guard lk(g_mu);
  for (auto& [s, q] : g_queues)
    if (s == sc) return q;
  return g_queues.empty() ? nullptr : g_queues.back().second;
}

}  // namespace tb
