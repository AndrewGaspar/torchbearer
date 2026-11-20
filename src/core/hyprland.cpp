// Hyprland / HypXRland IPC from inside Wine (core/hyprland.h).
//
// Verified 2026-11-19: a Windows program under GE-Proton 11-12 connects an AF_UNIX stream socket
// to Z:\run\user\1000\hypr\<signature>\.socket.sock and gets hyprctl's replies (j/version).
//
// Request grammar: a Lua-configured session evaluates `dispatch X` as `return hl.dispatch(X)`
// (HyprCtl.cpp dispatchRequest), so the tag is set with
//     dispatch hl.dsp.window.tag({ tag = "+name", window = "address:0x…" })
// and a classic (hyprlang) session takes `dispatch tagwindow +name address:0x…`. We try the Lua form
// and fall back on "Invalid dispatcher". `+name` sets, `-name` unsets (TagKeeper::applyTag).
#include "core/hyprland.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/lab.h"
#include "core/log.h"

namespace tb {
namespace {

struct SockaddrUn {
  unsigned short sun_family;
  char sun_path[108];
};

// The unix environment of this process (/proc/self/environ): Wine does not copy every variable
// into the Windows environment (XDG_RUNTIME_DIR is missing there under GE-Proton 11-12; live).
std::string unix_env(const char* name) {
  HANDLE f = CreateFileA("Z:\\proc\\self\\environ", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {};
  std::string all;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n > 0) all.append(buf, n);
  CloseHandle(f);
  std::string key = std::string(name) + "=";
  for (size_t p = 0; p < all.size();) {
    size_t e = all.find('\0', p);
    if (e == std::string::npos) e = all.size();
    if (all.compare(p, key.size(), key) == 0) return all.substr(p + key.size(), e - p - key.size());
    p = e + 1;
  }
  return {};
}

std::string env(const char* name) {
  char buf[512];
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
  if (n && n < sizeof buf) return std::string(buf, n);
  return unix_env(name);
}

std::string socket_path() {
  std::string sig = env("HYPRLAND_INSTANCE_SIGNATURE");
  if (sig.empty()) return {};
  std::string xdg = env("XDG_RUNTIME_DIR");
  if (xdg.empty()) return {};
  std::string p = "Z:" + xdg + "/hypr/" + sig + "/.socket.sock";
  for (auto& c : p)
    if (c == '/') c = '\\';
  return p;
}

// Find `"key": ` after `from` and before `end` in a JSON text; returns the position of the value.
size_t field(const std::string& j, const char* key, size_t from, size_t end) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = j.find(k, from);
  if (p == std::string::npos || p >= end) return std::string::npos;
  p += k.size();
  while (p < j.size() && j[p] == ' ') p++;
  return p;
}

struct Client {
  std::string address, title, stereo;
  int w = 0, h = 0, monitor = -1;
  bool mapped = false;
};

// All clients in `j/clients` (a JSON array of flat-ish objects; each starts with "address").
std::vector<Client> clients(const std::string& j) {
  std::vector<Client> out;
  size_t pos = 0;
  while ((pos = j.find("\"address\":", pos)) != std::string::npos) {
    size_t next = j.find("\"address\":", pos + 10);
    size_t end = next == std::string::npos ? j.size() : next;
    Client c;
    size_t a = field(j, "address", pos, end);
    if (a != std::string::npos && j[a] == '"') c.address = j.substr(a + 1, j.find('"', a + 1) - a - 1);
    size_t m = field(j, "mapped", pos, end);
    c.mapped = m != std::string::npos && j.compare(m, 4, "true") == 0;
    size_t t = field(j, "title", pos, end);
    if (t != std::string::npos && j[t] == '"') c.title = j.substr(t + 1, j.find('"', t + 1) - t - 1);
    size_t s = field(j, "stereo", pos, end);
    if (s != std::string::npos && j[s] == '"') c.stereo = j.substr(s + 1, j.find('"', s + 1) - s - 1);
    size_t mo = field(j, "monitor", pos, end);
    if (mo != std::string::npos) c.monitor = atoi(j.c_str() + mo);
    size_t z = field(j, "size", pos, end);
    if (z != std::string::npos && j[z] == '[') sscanf(j.c_str() + z, "[%d ,%d", &c.w, &c.h);
    if (!c.address.empty()) out.push_back(c);
    pos = end;
  }
  return out;
}

// This process's main window: the largest visible top-level window it owns.
HWND main_window() {
  struct Best {
    HWND h = nullptr;
    long area = 0;
  } best;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{};
        GetClientRect(h, &r);
        long area = long(r.right - r.left) * long(r.bottom - r.top);
        auto* b = reinterpret_cast<Best*>(lp);
        if (area > b->area) b->h = h, b->area = area;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&best));
  return best.h;
}

// Which compositor window is ours? The pid can't tell: under Wine, files are opened by wineserver,
// so Z:\proc\self is wineserver's (live: the pid read that way was wineserver's, the window's was
// the game process's). Instead the window's title briefly carries a random marker; the client
// whose title shows it is ours. The title is restored as soon as the compositor has seen it.
std::string identify(HWND hwnd) {
  wchar_t old[512] = {};
  GetWindowTextW(hwnd, old, 512);
  wchar_t marked[600];
  unsigned nonce = GetTickCount() ^ (GetCurrentProcessId() << 12) ^ unsigned(uintptr_t(hwnd));
  char tag[32];
  snprintf(tag, sizeof tag, "[tb-%08x]", nonce);
  _snwprintf(marked, 600, L"%ls %hs", old, tag);
  DWORD_PTR res = 0;
  if (!SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(marked), SMTO_ABORTIFHUNG, 3000, &res))
    return {};
  std::string found;
  for (int i = 0; i < 30 && found.empty(); i++) {
    Sleep(100);
    for (const auto& c : clients(hypr_request("j/clients")))
      if (c.title.find(tag) != std::string::npos) found = c.address;
  }
  SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(old), SMTO_ABORTIFHUNG, 3000, &res);
  return found;
}

// Monitors in `j/monitors`: HypXRland reports `"stereo": "sbs"` only on a side-by-side stereo
// output (an XReal in 3D mode under `monitor = …, stereo:sbs`), which is what the Quiver app's
// "3D on stereo displays: Auto" keys on too (quiver-app stereo_ui.rs).
struct Monitor {
  int id = -1;
  bool focused = false, stereo = false;
};
std::vector<Monitor> monitors(const std::string& j) {
  // Each monitor object has a "description" (its nested workspace objects, which also carry an
  // "id", don't): a monitor spans from the "id" before its description to the "id" before the
  // next monitor's description.
  std::vector<size_t> starts;
  for (size_t d = j.find("\"description\":"); d != std::string::npos; d = j.find("\"description\":", d + 1)) {
    size_t id = j.rfind("\"id\":", d);
    if (id != std::string::npos) starts.push_back(id);
  }
  std::vector<Monitor> out;
  for (size_t i = 0; i < starts.size(); i++) {
    size_t pos = starts[i], end = i + 1 < starts.size() ? starts[i + 1] : j.size();
    Monitor m;
    size_t v = field(j, "id", pos, end);
    if (v != std::string::npos) m.id = atoi(j.c_str() + v);
    size_t f = field(j, "focused", pos, end);
    m.focused = f != std::string::npos && j.compare(f, 4, "true") == 0;
    size_t st = field(j, "stereo", pos, end);
    m.stereo = st != std::string::npos && j.compare(st, 5, "\"sbs\"") == 0;
    out.push_back(m);
  }
  return out;
}

// -1 unknown (not Hyprland / no answer), 0 the game's monitor is mono, 1 it is a stereo output.
std::atomic<int> g_display_stereo{-1};

// Our window's monitor (when we know our window), else the focused one.
int display_stereo(const std::vector<Client>& all, const std::string& address) {
  auto ms = monitors(hypr_request("j/monitors"));
  if (ms.empty()) return -1;
  int mon = -1;
  for (const auto& c : all)
    if (!address.empty() && c.address == address) mon = c.monitor;
  for (const auto& m : ms)
    if ((mon >= 0 && m.id == mon) || (mon < 0 && m.focused)) return m.stereo ? 1 : 0;
  return -1;
}

std::mutex g_mu;
std::string g_want;     // tag to keep on the window ("" = none)
std::string g_applied;  // tag currently applied
std::string g_address;  // window it is applied to
std::string g_last;     // last outcome, for status
std::atomic<bool> g_thread{false};

std::string tag_request(const std::string& sign_tag, const std::string& address) {
  std::string r = hypr_request("dispatch hl.dsp.window.tag({ tag = \"" + sign_tag + "\", window = \"address:" +
                               address + "\" })");
  if (r.find("Invalid dispatcher") != std::string::npos)  // classic config
    r = hypr_request("dispatch tagwindow " + sign_tag + " address:" + address);
  return r;
}

void step() {
  std::string want, address;
  {
    std::lock_guard lk(g_mu);
    want = g_want;
    address = g_address;
  }
  // Is the window we know still there?
  const Client* me = nullptr;
  auto all = clients(hypr_request("j/clients"));
  int ds = display_stereo(all, address);
  if (ds != g_display_stereo.exchange(ds))
    TB_LOG("hyprland: the game's display is %s", ds == 1 ? "a stereo (side-by-side) output" : ds == 0 ? "mono" : "unknown");
  for (const auto& c : all)
    if (!address.empty() && c.address == address) me = &c;
  if (!me) {
    HWND h = main_window();
    if (!h) return;
    address = identify(h);
    if (address.empty()) return;
    all = clients(hypr_request("j/clients"));
    for (const auto& c : all)
      if (c.address == address) me = &c;
    if (!me) return;
    std::lock_guard lk(g_mu);
    if (address != g_address) g_applied.clear();  // a new window carries no tag of ours yet
    g_address = address;
  }
  std::lock_guard lk(g_mu);
  if (g_applied == want) {
    g_last = "window " + me->address + " \"" + me->title + "\" " + std::to_string(me->w) + "x" + std::to_string(me->h) +
             ", tag " + (want.empty() ? "(none)" : want) + ", compositor stereo: " + (me->stereo.empty() ? "?" : me->stereo);
    return;
  }
  if (!g_applied.empty()) tag_request("-" + g_applied, g_address);
  std::string r = want.empty() ? "ok" : tag_request("+" + want, g_address);
  TB_LOG("hyprland: window %s (\"%s\", %dx%d): tag %s -> %s", me->address.c_str(), me->title.c_str(), me->w, me->h,
         want.empty() ? "(cleared)" : want.c_str(), r.substr(0, 120).c_str());
  g_applied = want;
}

void thread_main() {
  for (;;) {
    step();
    Sleep(1000);
  }
}

}  // namespace

bool hypr_available() { return !socket_path().empty(); }

std::string hypr_request(const std::string& req) {
  static bool wsa = [] {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
  }();
  std::string path = socket_path();
  if (!wsa || path.empty() || path.size() >= sizeof(SockaddrUn::sun_path)) return {};
  SOCKET s = socket(1 /* AF_UNIX */, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return {};
  SockaddrUn a{};
  a.sun_family = 1;
  memcpy(a.sun_path, path.c_str(), path.size());
  std::string out;
  DWORD tmo = 2000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
      send(s, req.data(), int(req.size()), 0) == int(req.size())) {
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, size_t(n));
  }
  closesocket(s);
  return out;
}

void hypr_start() {
  if (g_thread.exchange(true)) return;
  if (!hypr_available()) {
    TB_LOG("hyprland: not running under Hyprland (no HYPRLAND_INSTANCE_SIGNATURE)");
    return;  // g_thread stays true: don't retry
  }
  std::thread(thread_main).detach();
}

void hypr_set_window_tag(const std::string& tag) {
  {
    std::lock_guard lk(g_mu);
    g_want = tag;
  }
  if (!tag.empty()) TB_LOG("hyprland: keeping window tag %s on this process's window", tag.c_str());
  hypr_start();
}

int hypr_display_stereo() { return g_display_stereo.load(); }

std::string process_env(const char* name) { return env(name); }

int hypr_display_stereo_now() {
  if (!hypr_available()) return -1;
  std::string address;
  {
    std::lock_guard lk(g_mu);
    address = g_address;
  }
  int ds = display_stereo(clients(hypr_request("j/clients")), address);
  g_display_stereo = ds;
  return ds;
}

std::string hypr_status() {
  std::lock_guard lk(g_mu);
  return "{\"available\":" + std::string(hypr_available() ? "true" : "false") +
         ",\"display_stereo\":" + std::to_string(g_display_stereo.load()) + ",\"want\":" + json_str(g_want) +
         ",\"applied\":" + json_str(g_applied) + ",\"address\":" + json_str(g_address) + ",\"last\":" + json_str(g_last) +
         "}";
}

}  // namespace tb
