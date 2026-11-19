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

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

CreateSwapChain_t o_CreateSwapChain;
CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd;
Present_t o_Present;
Present1_t o_Present1;
ResizeBuffers_t o_ResizeBuffers;

std::mutex g_mu;
std::vector<PresentFn> g_present;
std::vector<ResizeFn> g_resize;
std::atomic<uint64_t> g_frame{0};

void on_present(IDXGISwapChain* sc) {
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
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST)) on_present(sc);
  return o_Present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* p) {
  if (!(flags & DXGI_PRESENT_TEST)) on_present(sc);
  return o_Present1(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt,
                                           UINT flags) {
  HRESULT hr = o_ResizeBuffers(sc, n, w, h, fmt, flags);
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

void hook_swapchain(IDXGISwapChain* sc) {
  o_Present = reinterpret_cast<Present_t>(hook_vtable(sc, kSwapPresent, (void*)hk_Present));
  o_ResizeBuffers =
      reinterpret_cast<ResizeBuffers_t>(hook_vtable(sc, kSwapResizeBuffers, (void*)hk_ResizeBuffers));
  IDXGISwapChain1* sc1 = nullptr;
  if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
    o_Present1 = reinterpret_cast<Present1_t>(hook_vtable(sc1, kSwap1Present1, (void*)hk_Present1));
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
  HRESULT hr = o_CreateSwapChain(f, dev, desc, out);
  TB_LOG("CreateSwapChain = 0x%lx", hr);
  if (SUCCEEDED(hr) && out && *out) hook_swapchain(*out);
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                    IDXGIOutput* out, IDXGISwapChain1** sc) {
  HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, sc);
  TB_LOG("CreateSwapChainForHwnd(%ux%u fmt %d, %u buffers) = 0x%lx", desc ? desc->Width : 0,
         desc ? desc->Height : 0, desc ? int(desc->Format) : -1, desc ? desc->BufferCount : 0, hr);
  if (SUCCEEDED(hr) && sc && *sc) hook_swapchain(*sc);
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

}  // namespace tb
