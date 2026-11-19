#include "core/present.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

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

// --- stall watchdog ------------------------------------------------------------------------------
// If the presenting thread stops presenting for kStallMs, log where it is: its instruction pointer
// and the code addresses found on its stack, as module+offset. (2026-11-19: a real launch stopped
// presenting a few seconds after the main menu with every thread asleep, unreproducible in the lab,
// and a debugger attach killed the process.) Reported once per stall; reads only, the thread is
// suspended for the duration of one GetThreadContext and a stack copy.
constexpr DWORD kStallMs = 5000;
std::atomic<DWORD> g_present_tick{0};
std::atomic<DWORD> g_present_tid{0};
std::atomic<DWORD> g_test_stall_ms{0};  // lab verb `stall MS`: sleep in the next Present (tests the watchdog)

std::string where(uintptr_t a) {
  if (a < 0x100000) return {};  // small integers, not code
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(a), &m) ||
      !m)
    return {};
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
  std::wstring w(path, n);
  size_t slash = w.find_last_of(L"\\/");
  std::wstring base = slash == std::wstring::npos ? w : w.substr(slash + 1);
  uintptr_t off = a - reinterpret_cast<uintptr_t>(m);
  if (off < 0x1000) return {};  // the module base / headers: data, not a return address
  if (m == reinterpret_cast<HMODULE>(&__ImageBase)) base = L"torchbearer";
  char b[160];
  snprintf(b, sizeof b, "%ls+%#llx", base.c_str(), (unsigned long long)off);
  return b;
}

void report_stall(DWORD tid, DWORD since_ms) {
  HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!t) {
    TB_LOG("stall: no present for %lu ms; presenting thread %lu not accessible", since_ms, tid);
    return;
  }
  CONTEXT c{};
  c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  uintptr_t stack[512];
  size_t got = 0;
  bool ok = false;
  if (SuspendThread(t) != DWORD(-1)) {
    ok = GetThreadContext(t, &c);
    if (ok) {
      SIZE_T rd = 0;
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), stack, sizeof stack, &rd);
      got = rd / sizeof(uintptr_t);
    }
    ResumeThread(t);
  }
  CloseHandle(t);
  if (!ok) {
    TB_LOG("stall: no present for %lu ms; could not read thread %lu", since_ms, tid);
    return;
  }
  TB_LOG("stall: no present for %lu ms; presenting thread %lu at rip %s (%#llx), rsp %#llx", since_ms, tid,
         where(c.Rip).c_str(), (unsigned long long)c.Rip, (unsigned long long)c.Rsp);
  int shown = 0;
  for (size_t i = 0; i < got && shown < 24; i++) {
    std::string w = where(stack[i]);
    if (w.empty()) continue;
    TB_LOG("stall:   [rsp+%#zx] %s", i * sizeof(uintptr_t), w.c_str());
    shown++;
  }
}

void watchdog() {
  bool reported = false;
  for (;;) {
    Sleep(1000);
    DWORD last = g_present_tick.load(), tid = g_present_tid.load();
    if (!last || !tid) continue;
    DWORD since = GetTickCount() - last;
    if (since >= kStallMs && !reported) {
      reported = true;
      report_stall(tid, since);
    } else if (since < kStallMs && reported) {
      reported = false;
      TB_LOG("stall: presenting again");
    }
  }
}

// Returns false if a filter vetoed the real Present.
bool on_present(IDXGISwapChain* sc) {
  uint64_t f = g_frame.fetch_add(1) + 1;
  g_present_tick = GetTickCount();
  if (f == 1) {
    TB_LOG("first present");
    lab_state("running", {"frame=1"});
    g_present_tid = GetCurrentThreadId();
    CloseHandle(CreateThread(nullptr, 0, [](void*) -> DWORD { watchdog(); return 0; }, nullptr, 0, nullptr));
    lab_verb("stall", [](const std::vector<std::string>& a, bool& ok) -> std::string {
      g_test_stall_ms = a.empty() ? 7000 : DWORD(atoi(a[0].c_str()));
      return json_str("the next Present sleeps " + std::to_string(g_test_stall_ms.load()) + " ms");
    });
  }
  if (DWORD ms = g_test_stall_ms.exchange(0)) Sleep(ms);
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
unsigned present_width_scale_pending() { return g_width_scale.load(); }

void* present_queue(IDXGISwapChain* sc) {
  std::lock_guard lk(g_mu);
  for (auto& [s, q] : g_queues)
    if (s == sc) return q;
  return g_queues.empty() ? nullptr : g_queues.back().second;
}

}  // namespace tb
