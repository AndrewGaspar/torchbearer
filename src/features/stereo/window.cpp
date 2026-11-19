// Full-SBS window: the real window is twice the engine's width, the engine keeps one eye's size.
//
// The game's window object (app::WindowX86, global pointer at RVA 0xd0ad38; static, docs/stereo.md
// "Window") keeps the client size it renders at in +0x20/+0x24 and the window mode in +0x30
// (0 fullscreen popup, 1 windowed, 2 borderless popup, 3 maximised). It learns the size from
// WM_SIZE in its window procedure (0x3b39e0; a change sets +0x138 = 3, the engine's resize
// request) and from its own create (0x3b4b60) / resize (0x3b50b0) calls. Mouse messages carry
// client pixels that the input object normalises with +0x20/+0x24.
//
// So, with output = full:
//   - after create/resize, the real window is made 2x wide (client 2W x H), the object keeps W;
//   - WM_SIZE reaches the game with half the real width;
//   - absolute mouse positions reach the game as x mod W: over either half of the window the
//     game sees the position within that eye, so the OS cursor lines up with the menu in the eye
//     it is drawn over. Relative (captured) mouse mode is left alone;
//   - the normalised cursor getter (0x3b6b10, GetCursorPos / GetClientRect) gets the same mapping.
// The swapchain is already created 2x wide by core/present (width scale 2).
#include "features/stereo/window.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/hook.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"

namespace tb::stereo {
namespace {

constexpr uintptr_t kRvaWindowCreate = 0x3b4b60;  // WindowX86::create(this, title, w, h, mode, monitor, flags)
constexpr uintptr_t kRvaWindowResize = 0x3b50b0;  // WindowX86::resize(this, w, h, mode, monitor, flags)
constexpr uintptr_t kRvaWndProc = 0x3b39e0;       // the AppWindowClass window procedure
constexpr uintptr_t kRvaCursorNorm = 0x3b6b10;    // Mouse::cursor(this, float out[2]) in 0..1
constexpr uintptr_t kRvaWindowGlobal = 0xd0ad38;  // WindowX86* (set on WM_CREATE)
// Borderless (mode 2) takes its resolution from the monitor, not from the player's setting: the
// boot code in Game::Game (call at 0x1ce7aa, "Desktop dimensions") and Game::setDisplay (0x1d0e40,
// call at 0x1d0e8b) read it with MonitorModes::get (0x3b4590, out = {w, h, ...}) and store it as
// the resolution the swapchain and renderer use. While those two read it for borderless, the mod
// reports half the monitor's width, so the engine renders one eye and the window keeps the
// monitor's full width.
constexpr uintptr_t kRvaMonitorMode = 0x3b4590;
constexpr uintptr_t kRvaRetBoot = 0x1ce7af;        // after the call in Game::Game
constexpr uintptr_t kRvaRetSetDisplay = 0x1d0e90;  // after the call in Game::setDisplay (mode 2 only)
constexpr uintptr_t kRvaGamePtr = 0xb05dd0;        // Game* (docs/settings.md)
constexpr size_t kGameDisplayMode = 0xdc;          // 0 fullscreen, 1 windowed, 2 borderless
constexpr size_t kWinHwnd = 0x18, kWinW = 0x20, kWinH = 0x24, kWinMode = 0x30, kWinInput = 0x10;
constexpr size_t kInputRelative = 0x10;  // int: 0 = absolute cursor, else captured/relative

struct Sig {
  uintptr_t rva;
  const char* name;
  uint8_t bytes[16];
};
constexpr Sig kWinSigs[] = {
    {kRvaWindowCreate, "WindowX86::create",
     {0x48, 0x8b, 0xc4, 0x44, 0x89, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55}},
    {kRvaWindowResize, "WindowX86::resize",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48}},
    {kRvaWndProc, "WindowX86 wndproc",
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d}},
    {kRvaMonitorMode, "MonitorModes::get",
     {0x48, 0x83, 0x3d, 0x80, 0x67, 0x95, 0x00, 0x00, 0x74, 0x37, 0x85, 0xc9, 0x78, 0x33, 0x49, 0x63}},
    {kRvaCursorNorm, "Mouse::cursor",
     {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xfa}},
};

using Create_t = void(__fastcall*)(void*, const char*, int, int, int, int, uint8_t);
using Resize_t = void(__fastcall*)(void*, int, int, int, int, uint8_t);
using CursorNorm_t = float*(__fastcall*)(void*, float*);
Create_t o_create;
Resize_t o_resize;
WNDPROC o_wndproc;
CursorNorm_t o_cursor;
using MonitorMode_t = uint64_t(__fastcall*)(int, int*, int);
MonitorMode_t o_monitorMode;

std::atomic<bool> g_active{false};
std::atomic<int> g_widened{0}, g_sizes{0}, g_mouse{0};
int g_last_real_w = 0, g_last_engine_w = 0, g_last_h = 0;

template <typename T>
T& at(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}
void* window_obj() { return *reinterpret_cast<void**>(game_image().at(kRvaWindowGlobal)); }

bool active() { return g_active.load(std::memory_order_relaxed); }

// The engine's size (one eye), as the game last asked for it in create/resize. The real client
// is 2x that, unless that would not fit the screen: then the request is the real size and the
// engine gets half (borderless/fullscreen at a 3840x1080 desktop -> 1920x1080 per eye).
std::atomic<int> g_engine_w{0}, g_engine_h{0};
std::atomic<bool> g_rewiden{false};

void set_engine_size(int w, int h) {
  int screen = GetSystemMetrics(SM_CXSCREEN);
  int ew = (screen > 0 && 2 * w > screen) ? w / 2 : w;
  g_engine_w = ew;
  g_engine_h = h;
}

// What WM_SIZE tells the game for a real client width.
int engine_w(int real) {
  int ew = g_engine_w.load();
  if (ew > 0 && real == 2 * ew) return ew;
  if (ew > 0 && real == ew) {  // something set the window to the engine's size: widen it again
    g_rewiden = true;
    return ew;
  }
  // Any other size (the player resized the window, maximise, a desktop change): eyes are half.
  ew = real / 2;
  if (ew > 0) g_engine_w = ew;
  return ew;
}

// Make the real window 2x the engine's width, keeping the engine's size in the object.
void widen(void* win) {
  if (!win) return;
  HWND hwnd = at<HWND>(win, kWinHwnd);
  int w = g_engine_w.load(), h = g_engine_h.load(), mode = at<int>(win, kWinMode);
  if (!hwnd || w <= 0 || h <= 0) return;
  RECT cr{};
  GetClientRect(hwnd, &cr);
  int want = 2 * w;
  if (cr.right - cr.left != want || cr.bottom - cr.top != h) {
    DWORD style = DWORD(GetWindowLongW(hwnd, GWL_STYLE)), ex = DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE));
    RECT r{0, 0, want, h};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int cx = r.right - r.left, cy = r.bottom - r.top;
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int x = wr.left, y = wr.top;
    if (mode == 1) {  // windowed: the game centres it on the primary display; keep that
      x = (GetSystemMetrics(SM_CXSCREEN) - cx) / 2;
      y = (GetSystemMetrics(SM_CYSCREEN) - cy) / 2;
      if (x < 0) x = 0;
      if (y < 0) y = 0;
    }
    SetWindowPos(hwnd, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    g_widened++;
    TB_LOG("stereo window: mode %d, engine %dx%d, window client -> %dx%d (outer %dx%d at %d,%d)", mode, w, h, want,
           h, cx, cy, x, y);
  }
  at<int>(win, kWinW) = w;
  at<int>(win, kWinH) = h;
}

void __fastcall hk_create(void* win, const char* title, int w, int h, int mode, int monitor, uint8_t flags) {
  if (active()) {
    set_engine_size(w, h);
    TB_LOG("stereo window: create %dx%d mode %d monitor %d -> engine %dx%d", w, h, mode, monitor, g_engine_w.load(),
           h);
  }
  o_create(win, title, w, h, mode, monitor, flags);
  if (active()) widen(win);
}
void __fastcall hk_resize(void* win, int w, int h, int mode, int monitor, uint8_t flags) {
  if (active()) {
    set_engine_size(w, h);
    TB_LOG("stereo window: resize %dx%d mode %d monitor %d (from %#llx) -> engine %dx%d", w, h, mode, monitor,
           (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - game_image().base),
           g_engine_w.load(), h);
  }
  o_resize(win, w, h, mode, monitor, flags);
  if (active()) widen(win);
}

bool absolute_mouse() {
  void* win = window_obj();
  void* input = win ? at<void*>(win, kWinInput) : nullptr;
  return input && at<int>(input, kInputRelative) == 0;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (active()) {
    if (msg == WM_SIZE) {
      int real = LOWORD(lp), h = HIWORD(lp);
      int ew = engine_w(real);
      if (h > 0) g_engine_h = h;
      g_last_real_w = real, g_last_engine_w = ew, g_last_h = h;
      g_sizes++;
      lp = MAKELPARAM(ew, h);
    } else if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK && absolute_mouse()) {
      void* win = window_obj();
      int ew = win ? at<int>(win, kWinW) : 0;
      int x = short(LOWORD(lp)), y = short(HIWORD(lp));
      if (ew > 0 && x >= 0) x %= ew;
      lp = MAKELPARAM(x, y);
      g_mouse++;
    }
  }
  return o_wndproc(hwnd, msg, wp, lp);
}

float* __fastcall hk_cursor(void* mouse, float* out) {
  float* r = o_cursor(mouse, out);
  if (active() && out && out[0] >= 0.f && out[0] <= 1.f) {
    float x = out[0] * 2.f;
    out[0] = x - std::floor(x);  // position within the eye the cursor is over
  }
  return r;
}

uint64_t __fastcall hk_monitorMode(int idx, int* out, int monitor) {
  uint64_t r = o_monitorMode(idx, out, monitor);
  if (!active() || !out || !(r & 0xff)) return r;
  uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - game_image().base;
  bool borderless = false;
  if (ret == kRvaRetSetDisplay) {
    borderless = true;
  } else if (ret == kRvaRetBoot) {
    auto* game = *reinterpret_cast<uint8_t**>(game_image().at(kRvaGamePtr));
    borderless = game && at<int>(game, kGameDisplayMode) == 2;
    if (!game) TB_LOG("stereo window: boot desktop query before the Game pointer is set; left alone");
  }
  if (borderless && out[0] > 1) {
    static int logged_w = 0;
    if (logged_w != out[0]) TB_LOG("stereo window: borderless at %dx%d desktop -> engine resolution %dx%d", out[0], out[1], out[0] / 2,
           out[1]);
    logged_w = out[0];
    out[0] /= 2;
  }
  return r;
}

bool check_sigs() {
  const auto& img = game_image();
  for (const auto& s : kWinSigs) {
    if (memcmp(reinterpret_cast<const void*>(img.at(s.rva)), s.bytes, sizeof s.bytes) != 0) {
      TB_LOG("stereo window: signature mismatch for %s at RVA %#llx; full-SBS window disabled", s.name,
             (unsigned long long)s.rva);
      return false;
    }
  }
  return true;
}

}  // namespace

void window_start() {
  if (!check_sigs()) return;
  const auto& img = game_image();
  bool ok = hook_fn((void*)img.at(kRvaWindowCreate), (void*)hk_create, (void**)&o_create, "WindowX86::create") &&
            hook_fn((void*)img.at(kRvaWindowResize), (void*)hk_resize, (void**)&o_resize, "WindowX86::resize") &&
            hook_fn((void*)img.at(kRvaWndProc), (void*)hk_wndproc, (void**)&o_wndproc, "WindowX86 wndproc") &&
            hook_fn((void*)img.at(kRvaCursorNorm), (void*)hk_cursor, (void**)&o_cursor, "Mouse::cursor") &&
            hook_fn((void*)img.at(kRvaMonitorMode), (void*)hk_monitorMode, (void**)&o_monitorMode,
                    "MonitorModes::get");
  if (!ok) {
    TB_LOG("stereo window: hooks failed; full-SBS window disabled");
    return;
  }
  // output = full is read at start-up (the swapchain is created with it), and so is this.
  g_active = present_width_scale_pending() > 1;
  void* win = window_obj();
  TB_LOG("stereo window: hooks installed, %s; window object %p at start", active() ? "active (output = full)" : "inactive",
         win);
  if (active() && win) {  // the window may already exist
    set_engine_size(at<int>(win, kWinW), at<int>(win, kWinH));
    g_rewiden = true;  // on the game thread, once the hooks are enabled
  }
  // Re-widen on the game thread (its window) when something collapsed the window.
  present_on([](IDXGISwapChain*, uint64_t) {
    if (active() && g_rewiden.exchange(false)) widen(window_obj());
  });
}

std::string window_status() {
  char b[256];
  snprintf(b, sizeof b,
           "{\"active\":%s,\"engine\":\"%dx%d\",\"widened\":%d,\"wm_size\":%d,\"mouse\":%d,\"last_real_w\":%d,\"last_engine_w\":%d,"
           "\"last_h\":%d}",
           active() ? "true" : "false", g_engine_w.load(), g_engine_h.load(), g_widened.load(), g_sizes.load(), g_mouse.load(), g_last_real_w,
           g_last_engine_w, g_last_h);
  return b;
}

}  // namespace tb::stereo
