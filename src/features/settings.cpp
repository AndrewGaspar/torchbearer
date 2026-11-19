// The game's own settings, live (docs/settings.md).
//
// Every option of AWR's options menu lives either in GameProperties (a singleton the game saves to
// Documents/Remedy/AlanWakeRemastered/config) or in renderer globals (also saved there), or in the
// Game object (resolution.xml), or in the keybind map (keybinds.xml). The menu applies a change by
// writing those and calling the game's own apply functions; we do exactly the same, on the game's
// main thread (a hook on its per-frame timer update), then save through the game's own savers so
// the game's files agree with us.
//
// Config ids: `game.<name>` and `keys.<action>`. An absent or empty value leaves the game's own
// value alone. Changes made in the game's own menu are reported back (quiver_report, persisted).
//
// All addresses are RVAs in Game_f_x64_EOS.exe (Epic build, PE timestamp 0x697455b7); each code
// address is checked against a byte signature at start, and a setting whose checks fail is
// disabled (logged), never written.
#include "features/settings.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hook.h"
#include "core/lab.h"
#include "core/log.h"
#include "core/module.h"
#include "core/present.h"
#include "core/quiver.h"

namespace tb {
namespace {

// ---- Addresses ---------------------------------------------------------------------------------

// Functions (static analysis, re/ghidra; names are ours).
constexpr uintptr_t kGetProps = 0x1da6b0;    // GameProperties* get(): mov rax,[rip+..]; ret
constexpr uintptr_t kGetGame = 0x1d00f0;     // Game* get(): mov rax,[rip+..]; ret
constexpr uintptr_t kPropsApply = 0x1dd930;  // GameProperties::apply(this, bool resetCamera)
constexpr uintptr_t kPropsSave = 0x1dca80;   // GameProperties::save(this) -> "config"
constexpr uintptr_t kQuality = 0x242a20;     // MenuMode::applyGraphicsQuality(menu, int); 0..2
                                             // never touch `menu`
constexpr uintptr_t kSetDisplay = 0x1d0e40;  // Game::setDisplay(game, w, h, mode, dlss, idx, hdr)
constexpr uintptr_t kSaveResXml = 0x1d0b10;  // Game::saveResolutionXml(game, bool reset)
constexpr uintptr_t kTimerTick = 0x1df8d0;   // GameTimer::update(): main thread, once a frame
constexpr uintptr_t kRebind = 0x3fe680;      // Input::rebind(InplaceString* action, int vk): what
                                             // the keybinds menu does (unbinds other users of vk)
constexpr uintptr_t kKeySave = 0x3fe920;     // Input::saveKeybinds() -> keybinds.xml
constexpr uintptr_t kEnglishSet = 0x3e5690;  // Locale::setForceEnglish(bool)
constexpr uintptr_t kEnglishGet = 0x3e5320;  // Locale::forceEnglish()
constexpr uintptr_t kMenuApply = 0x25b100;   // MenuMode::menuToSettings (only for its references)
constexpr uintptr_t kMenuRefresh = 0x25c4e0; // MenuMode::settingsToMenu(menu)
constexpr uintptr_t kGetMenu = 0x24d5f0;     // MenuMode* get(): mov rax,[rip+..]; ret

// Globals (data).
constexpr uintptr_t kPropsPtr = 0xb06ba0;     // GameProperties*
constexpr uintptr_t kGamePtr = 0xb05dd0;      // Game*
constexpr uintptr_t kMenuPtr = 0xb14400;      // MenuMode* (exists from the title screen on)
constexpr uintptr_t kDevicePtr = 0xda35e0;    // render device; vtable +0x368 = DLSS mode changed
constexpr uintptr_t kVsync = 0x95566c;        // int  "VSync Enabled"
constexpr uintptr_t kShadow = 0x955670;       // int  "Shadow Quality" 0..2
constexpr uintptr_t kMsaa = 0xd755d8;         // u8   "Antialiasing" (menu: MSAA)
constexpr uintptr_t kAniso = 0xd75618;        // int  "Anisotropic Level" 0/2/4/8/16
constexpr uintptr_t kFxaa = 0xd755d9;         // u8   FXAA
constexpr uintptr_t kVolumetric = 0x955688;   // int  volumetric light quality 0..2
constexpr uintptr_t kTerrain = 0xd7561c;      // int  terrain quality 0..2
constexpr uintptr_t kDlss = 0xd75620;         // int  "DLSS Mode" 0 off,1 UP,2 P,3 B,4 Q,5 DLAA
constexpr uintptr_t kTaa = 0x955694;          // int  menu "antialiasing" (TAA)
constexpr uintptr_t kTaa2 = 0x955636;         // u8   TAA enable (render side)
constexpr uintptr_t kResScale = 0x9556b8;     // float 0.5..1.0
constexpr uintptr_t kKeyMap = 0xd34230;       // std::map<int action, int vk>* (head)
constexpr uintptr_t kKeyNames = 0xd34270;     // std::map<InplaceString, int action>* (head)

// GameProperties fields (offsets from GameProperties*; GameProperties::apply and the menu code).
enum : uint32_t {
  P_BRIGHTNESS = 0x104, P_SUBTITLES = 0x108, P_SUB_SIZE = 0x10c, P_SUB_BG = 0x110, P_HUD = 0x114,
  P_HUD_SIZE = 0x118, P_MUSIC = 0x11c, P_EFFECTS = 0x120, P_SPEECH = 0x124, P_CINEMATIC = 0x128,
  P_INVERT_Y = 0x12c, P_INVERT_X = 0x12d, P_PAD_SENS = 0x130, P_VIBRATION = 0x134,
  P_PAD_SCHEME = 0x138, P_SWAP_STICKS = 0x13c, P_SWAP_TRIGGERS = 0x13d, P_COMMENTARY = 0x140,
  P_MODERN_CAMERA = 0x144, P_MOUSE_X = 0x1f0, P_MOUSE_Y = 0x1f4, P_DRAW_DISTANCE = 0x1fc,
  P_QUALITY = 0x200, P_FOV = 0x204, P_DIRECT_AIM = 0x208, P_MOTION_BLUR = 0x209,
  P_FILM_GRAIN = 0x20a,
};
// Game fields (resolution.xml).
enum : uint32_t { G_WIDTH = 0xd4, G_HEIGHT = 0xd8, G_MODE = 0xdc, G_DISPLAY = 0xe0, G_HDR = 0xe4 };

using ApplyFn = void (*)(void*, char);
using SaveFn = void (*)(void*);
using QualityFn = void (*)(void*, int);
using SetDisplayFn = void (*)(void*, int, int, int, int, int, char);
using SaveResFn = void (*)(void*, char);
using TickFn = void (*)();
using RebindFn = void (*)(const char* const*, int);
using KeySaveFn = void (*)();
using EnglishSetFn = void (*)(char);
using EnglishGetFn = char (*)();
using DeviceFn = void (*)(void*);
using MenuFn = void (*)(void*);

uintptr_t at(uintptr_t rva) { return game_image().at(rva); }
template <typename T>
T& g(uintptr_t rva) {
  return *reinterpret_cast<T*>(at(rva));
}

// ---- Signature checks --------------------------------------------------------------------------

bool bytes_at(uintptr_t rva, const char* hex) {
  auto* p = reinterpret_cast<const uint8_t*>(at(rva));
  for (const char* h = hex; *h;) {
    while (*h == ' ') h++;
    if (!*h) break;
    unsigned v;
    if (sscanf(h, "%2x", &v) != 1 || *p != v) return false;
    p++;
    h += 2;
  }
  return true;
}

// Does the function at `fn` (first `len` bytes) reference `target` through a rip-relative operand?
bool refs(uintptr_t fn, size_t len, uintptr_t target) {
  auto* p = reinterpret_cast<const uint8_t*>(at(fn));
  for (size_t i = 0; i + 4 <= len; i++) {
    int32_t d;
    memcpy(&d, p + i, 4);
    for (int k = 0; k <= 4; k++)  // immediate bytes after the displacement
      if (int64_t(fn + i + 4 + k) + d == int64_t(target)) return true;
  }
  return false;
}

// The 64-bit `mov rax,[rip+d]; ret` getter at `fn` loads `ptr`.
bool getter_of(uintptr_t fn, uintptr_t ptr) {
  if (!bytes_at(fn, "48 8b 05")) return false;
  int32_t d;
  memcpy(&d, reinterpret_cast<const void*>(at(fn + 3)), 4);
  return fn + 7 + d == ptr && bytes_at(fn + 7, "c3");
}

enum Check : uint32_t {
  C_TICK = 1, C_PROPS = 2, C_APPLY = 4, C_SAVE = 8, C_QUALITY = 0x10, C_GAME = 0x20,
  C_DISPLAY = 0x40, C_KEYS = 0x80, C_ENGLISH = 0x100, C_DLSS = 0x200, C_MENU = 0x400,
};
uint32_t g_checks = 0;  // passed

void run_checks() {
  auto check = [](Check c, const char* name, bool ok) {
    if (ok) g_checks |= c;
    TB_LOG("settings: check %-8s %s", name, ok ? "ok" : "MISMATCH (settings that need it are off)");
  };
  check(C_TICK, "tick", bytes_at(kTimerTick, "48 8b c4 53 48 81 ec 90 00 00 00 f3 0f 10 05"));
  check(C_PROPS, "props", getter_of(kGetProps, kPropsPtr));
  check(C_APPLY, "apply",
        bytes_at(kPropsApply, "48 89 5c 24 08 57 48 83 ec 30 0f b6 fa 48 8b d9 e8") &&
            bytes_at(kPropsApply + 0x15, "f3 0f 10 8b 1c 01 00 00"));  // movss xmm1,[rbx+0x11c]
  // The saver writes the renderer globals in this order: that pins every global we use.
  {
    const uintptr_t order[] = {kVsync, 0x955680, kShadow, 0x955684, 0x955674, kMsaa, 0x955690,
                               kAniso, kFxaa, 0x95567c, 0x955660, 0xd755e0, kVolumetric,
                               0x955698, 0x95569c, kTerrain, kDlss, kTaa, kResScale};
    bool ok = bytes_at(kPropsSave, "48 89 5c 24 18 48 89 7c 24 20 55 48 8d ac 24 40 fe ff ff");
    for (uintptr_t t : order) ok = ok && refs(kPropsSave, 0x360, t);
    check(C_SAVE, "save", ok);
  }
  {
    bool ok = bytes_at(kQuality, "48 8b c4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8d 68 a1");
    for (uintptr_t t : {kTaa2, uintptr_t(0x955653), uintptr_t(0x95565e), uintptr_t(0xd75615),
                        uintptr_t(0x955678), uintptr_t(0x9556a8), uintptr_t(0x9556ac),
                        uintptr_t(0x9556b0), uintptr_t(0x955649)})
      ok = ok && refs(kQuality, 0x800, t);
    check(C_QUALITY, "quality", ok);
  }
  check(C_GAME, "game", getter_of(kGetGame, kGamePtr));
  check(C_DISPLAY, "display",
        bytes_at(kSetDisplay, "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 48 89 7c 24 20 41 56") &&
            bytes_at(kSaveResXml, "48 89 5c 24 08 48 89 7c 24 10 55 48 8d ac 24 70 fe ff ff") &&
            refs(kSaveResXml, 0x200, kDlss));
  check(C_KEYS, "keys",
        bytes_at(kRebind, "40 53 48 83 ec 20 48 8b 09 8b da e8") &&
            refs(kRebind, 0x40, kKeyMap) &&
            bytes_at(kKeySave, "48 89 5c 24 08 48 89 74 24 10 48 89 7c 24 18 4c 89 74 24 20 55") &&
            refs(kKeySave, 0x100, kKeyMap) && refs(kKeySave, 0x100, kKeyNames));
  check(C_ENGLISH, "english",
        bytes_at(kEnglishSet, "40 53 48 83 ec 20 0f b6 d9 e8") &&
            bytes_at(kEnglishSet + 0x0e, "88 58 24") &&
            bytes_at(kEnglishGet, "48 83 ec 28 e8") && bytes_at(kEnglishGet + 9, "0f b6 40 24"));
  check(C_MENU, "menu",
        getter_of(kGetMenu, kMenuPtr) &&
            bytes_at(kMenuRefresh, "48 8b c4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55") &&
            refs(kMenuRefresh, 0x1800, kDlss));
  // MenuMode::menuToSettings: `if (dlss changed) device->vtbl[0x368/8]()`.
  check(C_DLSS, "dlss",
        refs(kMenuApply, 0x1400, kDevicePtr) && refs(kMenuApply, 0x1400, kDlss));
}

// ---- Game state --------------------------------------------------------------------------------

uint8_t* props() { return (g_checks & C_PROPS) ? g<uint8_t*>(kPropsPtr) : nullptr; }
uint8_t* game_obj() { return (g_checks & C_GAME) ? g<uint8_t*>(kGamePtr) : nullptr; }
template <typename T>
T& pf(uint32_t off) {
  return *reinterpret_cast<T*>(props() + off);
}
template <typename T>
T& gf(uint32_t off) {
  return *reinterpret_cast<T*>(game_obj() + off);
}

// ---- Value text --------------------------------------------------------------------------------

std::string num(double v) {
  char b[32];
  snprintf(b, sizeof b, "%g", v);
  return b;
}
bool parse_num(const std::string& s, double* out) {
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  *out = v;
  return true;
}
bool parse_bool(const std::string& s, bool* out) {
  std::string l;
  for (char c : s) l += char(tolower(static_cast<unsigned char>(c)));
  if (l == "1" || l == "true" || l == "on" || l == "yes") return *out = true, true;
  if (l == "0" || l == "false" || l == "off" || l == "no") return *out = false, true;
  return false;
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- Settings table ----------------------------------------------------------------------------

enum Group : uint32_t {
  APPLY = 1,      // GameProperties::apply (volumes, input, brightness, anisotropy, aim, blur...)
  GRAPHICS = 2,   // graphics quality preset / custom values
  DLSS = 4,       // render device DLSS reconfigure (+ GRAPHICS for the AA interplay)
  DISPLAY = 8,    // Game::setDisplay
  KEYS = 16,      // keybinds.xml
  RESXML = 32,    // also save resolution.xml
};

struct Setting {
  std::string id;  // config id
  uint32_t needs;  // Check bits
  uint32_t group;  // Group bits to run after writing
  std::function<std::string()> get;
  std::function<bool(const std::string&)> set;  // false: value not understood
};
std::vector<Setting> g_settings;

// Display values wanted (filled from the game's current values on first use).
struct Display {
  bool init = false;
  int w = 0, h = 0, mode = 0, index = 0;
  bool hdr = false;
} g_disp;
void disp_init() {
  if (g_disp.init || !game_obj()) return;
  g_disp = {true, gf<int>(G_WIDTH), gf<int>(G_HEIGHT), gf<int>(G_MODE), gf<int>(G_DISPLAY),
            gf<uint8_t>(G_HDR) != 0};
}

// Slider (float 0..1, step 0.05 in the menu) shown as a percentage 0..100.
void pct(const char* id, uint32_t off, uint32_t group) {
  g_settings.push_back({std::string("game.") + id, C_PROPS, group,
                        [off] { return num(std::lround(pf<float>(off) * 100.0)); },
                        [off](const std::string& s) {
                          double v;
                          if (!parse_num(s, &v)) return false;
                          v = v < 0 ? 0 : v > 100 ? 100 : v;
                          pf<float>(off) = float(v / 100.0);
                          return true;
                        }});
}
void boolp(const char* id, uint32_t off, uint32_t group) {
  g_settings.push_back({std::string("game.") + id, C_PROPS, group,
                        [off] { return std::string(pf<uint8_t>(off) ? "1" : "0"); },
                        [off](const std::string& s) {
                          bool b;
                          if (!parse_bool(s, &b)) return false;
                          pf<uint8_t>(off) = b;
                          return true;
                        }});
}
// An int read/written through accessors, shown as one of `names` (index = value), or the number.
void enumv(const char* id, uint32_t needs, uint32_t group, std::vector<std::string> names,
           std::function<int()> rd, std::function<void(int)> wr) {
  g_settings.push_back(
      {std::string("game.") + id, needs, group,
       [names, rd] {
         int v = rd();
         return v >= 0 && v < int(names.size()) ? names[v] : num(v);
       },
       [names, wr](const std::string& s) {
         for (size_t i = 0; i < names.size(); i++)
           if (s == names[i]) return wr(int(i)), true;
         double v;
         if (!parse_num(s, &v) || v < 0 || v >= names.size()) return false;
         wr(int(v));
         return true;
       }});
}
template <typename T>
void enum_glob(const char* id, uintptr_t rva, uint32_t group, std::vector<std::string> names) {
  enumv(id, C_SAVE, group, names, [rva] { return int(g<T>(rva)); },
        [rva](int v) { g<T>(rva) = T(v); });
}
void enum_prop(const char* id, uint32_t off, uint32_t group, std::vector<std::string> names) {
  enumv(id, C_PROPS, group, names, [off] { return pf<int>(off); },
        [off](int v) { pf<int>(off) = v; });
}

const std::vector<std::string> kLMH = {"low", "medium", "high"};
const char* kActions[] = {"MOVE_FORWARD", "MOVE_BACKWARD", "MOVE_LEFT", "MOVE_RIGHT", "TURBOBOOST",
                          "WALK", "USE", "RELOAD", "JUMP", "SHOOT", "THROW_ITEM", "LIGHT_BOOST",
                          "RELOAD_BATTERY", "LOOK_AT", "TOGGLE_MANUSCRIPT", "INVENTORY_RIGHT",
                          "INVENTORY_UP", "INVENTORY_LEFT", "INVENTORY_DOWN", "INVENTORY_EXTRA",
                          "TOGGLE_SIDE"};

// Keybinds in memory: std::map<int action, int vk> (MSVC tree: left +0, parent +8, right +0x10,
// isnil +0x19, key +0x1c, value +0x20) and std::map<InplaceString name, int action> (name at
// +0x20, its char* first; action at +0xc8). Walked read-only.
struct Node {
  Node* left;
  Node* parent;
  Node* right;
  char color, isnil;
};
template <typename F>
void walk(Node* head, F f) {
  if (!head) return;
  std::vector<Node*> stack{head->parent};
  int guard = 0;
  while (!stack.empty() && guard++ < 1024) {
    Node* n = stack.back();
    stack.pop_back();
    if (!n || n->isnil) continue;
    f(n);
    stack.push_back(n->left);
    stack.push_back(n->right);
  }
}
int action_id(const char* name) {
  int id = -1;
  walk(g<Node*>(kKeyNames), [&](Node* n) {
    const char* s = *reinterpret_cast<const char* const*>(reinterpret_cast<uint8_t*>(n) + 0x20);
    if (s && strcmp(s, name) == 0) id = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(n) + 0xc8);
  });
  return id;
}
int bound_vk(int action) {
  int vk = -1;
  walk(g<Node*>(kKeyMap), [&](Node* n) {
    if (*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(n) + 0x1c) == action)
      vk = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(n) + 0x20);
  });
  return vk;
}

void build_table() {
  // Picture and HUD.
  pct("brightness", P_BRIGHTNESS, APPLY);
  boolp("hud", P_HUD, 0);
  pct("hud_size", P_HUD_SIZE, 0);
  boolp("motion_blur", P_MOTION_BLUR, APPLY);
  boolp("film_grain", P_FILM_GRAIN, APPLY);
  // Field of view: the slider (0..1, default 0.5) scales the camera's FOV by 0.75 + slider/2
  // (FUN_1401d3200), shown as a percentage of the default FOV, 75..125.
  g_settings.push_back({"game.fov", C_PROPS, 0,
                        [] { return num(std::lround(75 + pf<float>(P_FOV) * 50)); },
                        [](const std::string& s) {
                          double v;
                          if (!parse_num(s, &v)) return false;
                          v = v < 75 ? 75 : v > 125 ? 125 : v;
                          pf<float>(P_FOV) = float((v - 75) / 50);
                          return true;
                        }});

  // Display (resolution.xml): applied through Game::setDisplay, like the menu's Apply.
  auto disp = [](const char* id, std::function<std::string()> rd,
                 std::function<bool(const std::string&)> wr) {
    g_settings.push_back({std::string("game.") + id, C_GAME | C_DISPLAY | C_SAVE, DISPLAY | RESXML,
                          rd, [wr](const std::string& s) {
                            disp_init();
                            return wr(s);
                          }});
  };
  disp("resolution",
       [] { return num(gf<int>(G_WIDTH)) + "x" + num(gf<int>(G_HEIGHT)); },
       [](const std::string& s) {
         int w, h;
         if (sscanf(s.c_str(), "%dx%d", &w, &h) != 2 || w < 320 || h < 200) return false;
         g_disp.w = w, g_disp.h = h;
         return true;
       });
  const std::vector<std::string> modes = {"fullscreen", "windowed", "borderless"};
  disp("display_mode",
       [modes] {
         int m = gf<int>(G_MODE);
         return m >= 0 && m < 3 ? modes[m] : num(m);
       },
       [modes](const std::string& s) {
         for (int i = 0; i < 3; i++)
           if (s == modes[i]) return g_disp.mode = i, true;
         double v;
         if (!parse_num(s, &v) || v < 0 || v > 2) return false;
         g_disp.mode = int(v);
         return true;
       });
  disp("monitor", [] { return num(gf<int>(G_DISPLAY)); },
       [](const std::string& s) {
         double v;
         if (!parse_num(s, &v) || v < 0 || v > 15) return false;
         g_disp.index = int(v);
         return true;
       });
  disp("hdr", [] { return std::string(gf<uint8_t>(G_HDR) ? "1" : "0"); },
       [](const std::string& s) {
         bool b;
         if (!parse_bool(s, &b)) return false;
         g_disp.hdr = b;
         return true;
       });

  // Rendering.
  enumv("vsync", C_SAVE, 0, {"0", "1"}, [] { return g<int>(kVsync) != 0; },
        [](int v) { g<int>(kVsync) = v; });
  enumv("dlss", C_SAVE | C_DLSS, DLSS | GRAPHICS | APPLY | RESXML,
        {"off", "ultra_performance", "performance", "balanced", "quality", "dlaa"},
        [] { return g<int>(kDlss); }, [](int v) { g<int>(kDlss) = v; });
  // Resolution scale: 50..100 % in steps of 10 (menu indices 0..5; scale = 0.5 + index / 10).
  g_settings.push_back({"game.resolution_scale", C_SAVE, 0,
                        [] { return num(std::lround(g<float>(kResScale) * 10) * 10); },
                        [](const std::string& s) {
                          double v;
                          if (!parse_num(s, &v)) return false;
                          int pctv = clampi(int(std::lround(v / 10.0)) * 10, 50, 100);
                          g<float>(kResScale) = float(pctv / 100.0);
                          return true;
                        }});
  enum_prop("quality", P_QUALITY, GRAPHICS | APPLY, {"low", "medium", "high", "custom"});
  // Custom-quality values (the game only uses them when quality = custom; the presets overwrite
  // them otherwise).
  enumv("taa", C_SAVE | C_QUALITY, GRAPHICS, {"0", "1"}, [] { return g<int>(kTaa) != 0; },
        [](int v) { g<int>(kTaa) = v, g<uint8_t>(kTaa2) = uint8_t(v); });
  enum_glob<uint8_t>("fxaa", kFxaa, GRAPHICS, {"0", "1"});
  enum_glob<uint8_t>("msaa", kMsaa, GRAPHICS, {"0", "1"});
  {
    const int levels[] = {0, 2, 4, 8, 16};
    enumv("anisotropy", C_SAVE | C_APPLY, GRAPHICS | APPLY, {"off", "2x", "4x", "8x", "16x"},
          [levels] {
            for (int i = 0; i < 5; i++)
              if (g<int>(kAniso) == levels[i]) return i;
            return 100 + g<int>(kAniso);  // not a menu level: shown as 100 + level
          },
          [levels](int i) { g<int>(kAniso) = levels[i]; });
  }
  enum_glob<int>("shadows", kShadow, GRAPHICS, kLMH);
  enum_glob<int>("volumetrics", kVolumetric, GRAPHICS, kLMH);
  enum_glob<int>("terrain", kTerrain, GRAPHICS, kLMH);
  pct("draw_distance", P_DRAW_DISTANCE, 0);

  // Audio.
  pct("music", P_MUSIC, APPLY);
  pct("effects", P_EFFECTS, APPLY);
  pct("speech", P_SPEECH, APPLY);
  pct("cinematic", P_CINEMATIC, APPLY);
  g_settings.push_back({"game.english_speech", C_ENGLISH, 0,
                        [] {
                          return std::string(reinterpret_cast<EnglishGetFn>(at(kEnglishGet))()
                                                 ? "1"
                                                 : "0");
                        },
                        [](const std::string& s) {
                          bool b;
                          if (!parse_bool(s, &b)) return false;
                          reinterpret_cast<EnglishSetFn>(at(kEnglishSet))(b);
                          return true;
                        }});

  // Subtitles and extras.
  boolp("subtitles", P_SUBTITLES, 0);
  pct("subtitle_size", P_SUB_SIZE, 0);
  pct("subtitle_background", P_SUB_BG, 0);
  enum_prop("commentary", P_COMMENTARY, 0, {"off", "original", "remaster"});

  // Mouse and keyboard.
  boolp("invert_y", P_INVERT_Y, APPLY);
  boolp("invert_x", P_INVERT_X, APPLY);
  pct("mouse_sensitivity_x", P_MOUSE_X, APPLY);
  pct("mouse_sensitivity_y", P_MOUSE_Y, APPLY);
  boolp("direct_aiming", P_DIRECT_AIM, APPLY);
  enum_prop("modern_camera", P_MODERN_CAMERA, APPLY, {"off", "recoil", "camera_and_recoil"});

  // Gamepad (the menu shows these on consoles only; the PC game still reads them).
  pct("gamepad_sensitivity", P_PAD_SENS, APPLY);
  boolp("vibration", P_VIBRATION, APPLY);
  enumv("gamepad_layout", C_PROPS, APPLY, {"1", "2"}, [] { return pf<int>(P_PAD_SCHEME) % 2; },
        [](int v) { pf<int>(P_PAD_SCHEME) = (pf<int>(P_PAD_SCHEME) / 2) * 2 + v; });
  enumv("driving_layout", C_PROPS, APPLY, {"1", "2", "3", "4"},
        [] { return (pf<int>(P_PAD_SCHEME) / 2) % 4; },
        [](int v) { pf<int>(P_PAD_SCHEME) = pf<int>(P_PAD_SCHEME) % 2 + v * 2; });
  boolp("swap_sticks", P_SWAP_STICKS, APPLY);
  boolp("swap_triggers", P_SWAP_TRIGGERS, APPLY);

  // Keybinds.
  for (const char* a : kActions) {
    std::string id = "keys.";
    for (const char* c = a; *c; c++) id += char(tolower(static_cast<unsigned char>(*c)));
    g_settings.push_back({id, C_KEYS, KEYS,
                          [a] {
                            int act = action_id(a);
                            int vk = act < 0 ? -1 : bound_vk(act);
                            return vk < 0 ? std::string() : key_to_name(unsigned(vk));
                          },
                          [a](const std::string& s) {
                            GameKey k;
                            if (!key_from_name(s, &k)) return false;
                            // rebind reads only the string's first field, its char*.
                            const char* name = a;
                            reinterpret_cast<RebindFn>(at(kRebind))(&name, int(k.vk_mem));
                            return true;
                          }});
  }
}

Setting* find(const std::string& id) {
  for (auto& s : g_settings)
    if (s.id == id) return &s;
  return nullptr;
}
bool usable(const Setting& s) { return (g_checks & s.needs) == s.needs; }

// ---- Applying ----------------------------------------------------------------------------------

std::mutex g_mu;
std::map<std::string, std::string> g_want;  // config values (non-empty), by id
std::set<std::string> g_dirty;              // ids changed in the config since the last tick
std::map<std::string, std::string> g_seen;  // game values as of our last look (for reporting)
uint64_t g_frame = 0, g_ready_at = 0, g_save_at = 0;
bool g_ready = false, g_save_res = false;

// Graphics quality, as MenuMode::applyGraphicsQuality does it: a preset for 0..2, else the
// custom values (already written) plus the fixed values the game sets for custom.
void apply_graphics() {
  int q = pf<int>(P_QUALITY);
  if (q >= 0 && q <= 2) {
    reinterpret_cast<QualityFn>(at(kQuality))(nullptr, q);  // `menu` is used only for custom
    return;
  }
  pf<int>(P_QUALITY) = 3;
  auto rf = [](uintptr_t rva) { return g<float>(rva); };
  g<uint8_t>(0x955653) = 1;   // "Render Shadows"
  g<uint8_t>(0x95565e) = 1;   // "Render Volumetric Lights"
  g<uint8_t>(0xd75615) = 0;   // "Force HD Textures"
  g<float>(0x955698) = rf(0x75bd80);  // "LOD Distance" (high)
  g<float>(0x95569c) = rf(0x74eb3c);  // "Grass Distance" (high)
  g<float>(0x9556ac) = rf(0x74e77c);  // "Foliage LOD Scale" (high)
  g<float>(0x9556b0) = rf(0x74e5f0);  // "Small Foliage Distance" (high)
  g<int>(0x955678) = 2;
  g<float>(0x9556a8) = rf(0x757750);  // "Interior LOD Distance" (high)
  g<uint8_t>(0xd755e0) = 0;           // shared tail of the preset function
  g<uint8_t>(0x955649) = 1;
  g<int>(0x95567c) = 2;
  g<int>(0x955674) = 3;
  g<int>(0x955684) = 2;
  g<uint8_t>(0x955660) = 1;
  g<int>(0x955680) = 2;
  if (g<int>(kDlss) != 0) {  // DLSS replaces the game's AA
    g<uint8_t>(kTaa2) = 0;
    g<int>(kTaa) = 0;
    g<uint8_t>(kMsaa) = 0;
  }
}

void apply_dlss() {
  void* dev = g<void*>(kDevicePtr);
  if (!dev) return;
  auto fn = reinterpret_cast<DeviceFn>((*reinterpret_cast<void***>(dev))[0x368 / 8]);
  fn(dev);
}

void apply_display() {
  disp_init();
  reinterpret_cast<SetDisplayFn>(at(kSetDisplay))(game_obj(), g_disp.w, g_disp.h, g_disp.mode,
                                                 g<int>(kDlss), g_disp.index, g_disp.hdr);
}

void snapshot_all() {
  for (auto& s : g_settings)
    if (usable(s)) g_seen[s.id] = s.get();
}

void apply_dirty() {
  std::map<std::string, std::string> todo;
  {
    std::lock_guard lk(g_mu);
    for (auto& id : g_dirty) {
      auto it = g_want.find(id);
      if (it != g_want.end()) todo[id] = it->second;
    }
    g_dirty.clear();
  }
  g_disp.init = false;  // display values start from the game's current ones in every batch
  uint32_t groups = 0;
  for (auto& [id, want] : todo) {
    Setting* s = find(id);
    if (!s || !usable(*s)) continue;
    std::string cur = s->get();
    if (cur == want) continue;
    if (!s->set(want)) {
      TB_LOG("settings: %s = '%s' not understood (kept %s)", id.c_str(), want.c_str(), cur.c_str());
      continue;
    }
    TB_LOG("settings: %s %s -> %s", id.c_str(), cur.c_str(), want.c_str());
    groups |= s->group | 0x80000000u;
  }
  if (!groups) return;
  if ((groups & DLSS) && (g_checks & C_DLSS)) apply_dlss();
  if ((groups & GRAPHICS) && (g_checks & C_QUALITY)) apply_graphics();
  if ((groups & (APPLY | GRAPHICS)) && (g_checks & C_APPLY))
    reinterpret_cast<ApplyFn>(at(kPropsApply))(props(), 0);
  if (groups & DISPLAY) apply_display();
  if (groups & KEYS) reinterpret_cast<KeySaveFn>(at(kKeySave))();
  if (groups & RESXML) g_save_res = true;
  g_save_at = g_frame + 30;  // the game's own save, once things settle
  // The options menu copies every item back into the settings on its next change; refresh its
  // items from the settings (what the game does when the menu opens), so it shows our values
  // and doesn't revert them.
  if ((g_checks & C_MENU) && g<void*>(kMenuPtr))
    reinterpret_cast<MenuFn>(at(kMenuRefresh))(g<void*>(kMenuPtr));
  snapshot_all();  // what we caused (presets, AA interplay) is not a game change
}

// Values the game changed by itself (its own menu): report them so Quiver keeps them.
void report_game_changes() {
  for (auto& s : g_settings) {
    if (!usable(s)) continue;
    std::string v = s.get();
    auto it = g_seen.find(s.id);
    if (it == g_seen.end()) {
      g_seen[s.id] = v;
      continue;
    }
    if (it->second == v) continue;
    it->second = v;
    {
      std::lock_guard lk(g_mu);
      auto w = g_want.find(s.id);
      if (w != g_want.end() && w->second == v) continue;  // ours, applied late (display mode)
    }
    TB_LOG("settings: game changed %s -> %s", s.id.c_str(), v.c_str());
    {
      std::lock_guard lk(g_mu);
      g_want[s.id] = v;  // so a later config event doesn't treat it as ours to undo
    }
    cfg_set(s.id, v, "game");
    quiver_report(s.id, v, true);
  }
}

TickFn g_tick_orig = nullptr;

void tick() {
  g_frame++;
  if (!g_ready) {
    if (!props()) return;
    if (!g_ready_at) g_ready_at = g_frame + 30;  // the game loads its files before the loop runs
    if (g_frame < g_ready_at) return;
    g_ready = true;
    snapshot_all();
    TB_LOG("settings: ready at frame %llu (GameProperties %p, Game %p)",
           (unsigned long long)g_frame, (void*)props(), (void*)game_obj());
    apply_dirty();
    // Show the game's own values for the settings the profile leaves alone (not saved).
    for (auto& s : g_settings) {
      if (!usable(s)) continue;
      std::lock_guard lk(g_mu);
      if (!g_want.count(s.id)) quiver_report(s.id, g_seen[s.id], false);
    }
  }
  apply_dirty();
  if (g_save_at && g_frame >= g_save_at) {
    g_save_at = 0;
    if (g_checks & C_SAVE) reinterpret_cast<SaveFn>(at(kPropsSave))(props());
    if (g_save_res && (g_checks & C_DISPLAY)) {
      reinterpret_cast<SaveResFn>(at(kSaveResXml))(game_obj(), 0);
      g_save_res = false;
    }
    TB_LOG("settings: saved the game's config");
  }
  if (g_frame % 30 == 0) report_game_changes();
}

void tick_detour() {
  g_tick_orig();
  tick();
}

// Display values are read from resolution.xml when the game starts, before its window exists;
// rewriting the file first (we run before the game's own code reads it, from the dxgi import) means
// the game starts in the wanted mode instead of switching once it's up. If we lose that race the
// live path still applies them when the game is ready.
std::wstring docs_dir() {
  using Fn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
  HMODULE sh = LoadLibraryW(L"shell32.dll");
  auto fn = sh ? reinterpret_cast<Fn>(GetProcAddress(sh, "SHGetFolderPathW")) : nullptr;
  wchar_t buf[MAX_PATH];
  if (!fn || fn(nullptr, 5 /* CSIDL_PERSONAL */, nullptr, 0, buf) != 0) return L"";
  return std::wstring(buf) + L"\\Remedy\\AlanWakeRemastered\\";
}

void prepatch_resolution_xml() {
  std::map<std::string, std::string> want;
  {
    std::lock_guard lk(g_mu);
    want = g_want;
  }
  std::vector<std::pair<std::string, std::string>> set;  // element -> value
  auto w = [&](const char* id) {
    auto it = want.find(id);
    return it == want.end() ? std::string() : it->second;
  };
  int x, y;
  if (sscanf(w("game.resolution").c_str(), "%dx%d", &x, &y) == 2 && x >= 320 && y >= 200)
    set.push_back({"width", std::to_string(x)}), set.push_back({"height", std::to_string(y)});
  const char* modes[] = {"fullscreen", "windowed", "borderless"};
  for (int i = 0; i < 3; i++)
    if (w("game.display_mode") == modes[i]) set.push_back({"window_mode", std::to_string(i)});
  const char* dlss[] = {"off", "ultra_performance", "performance", "balanced", "quality", "dlaa"};
  for (int i = 0; i < 6; i++)
    if (w("game.dlss") == dlss[i]) set.push_back({"dlss", std::to_string(i)});
  bool b;
  if (parse_bool(w("game.hdr"), &b)) set.push_back({"hdr", b ? "1" : "0"});
  if (set.empty()) return;

  std::wstring path = docs_dir() + L"resolution.xml";
  std::ifstream in{std::filesystem::path(path)};
  if (!in) return;  // the game writes a fresh one; the live path applies ours then
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::string xml = ss.str(), orig = xml;
  for (auto& [el, v] : set) {
    // Splice by hand: a format string "$1" + "1600" would read as group 11.
    std::smatch m;
    if (std::regex_search(xml, m, std::regex("<" + el + R"re(\s+value="([^"]*)")re")))
      xml.replace(size_t(m.position(1)), size_t(m.length(1)), v);
  }
  if (xml == orig) return;
  std::wstring tmp = path + L".tb-tmp";
  {
    std::ofstream out{std::filesystem::path(tmp), std::ios::binary | std::ios::trunc};
    out << xml;
  }
  bool ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  TB_LOG("settings: resolution.xml %s before the game read it", ok ? "updated" : "NOT updated");
}

}  // namespace

void settings_register() {
  build_table();
  cfg_listen("game.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_mu);
    if (v.empty())
      g_want.erase(id);
    else
      g_want[id] = v;
    g_dirty.insert(id);
  });
  cfg_listen("keys.", [](const std::string& id, const std::string& v) {
    std::lock_guard lk(g_mu);
    if (v.empty())
      g_want.erase(id);
    else
      g_want[id] = v;
    g_dirty.insert(id);
  });
  // `quiver lab call <i> game [id]`: the game's live value(s), read from its memory.
  lab_verb("game", [](const std::vector<std::string>& a, bool& ok) -> std::string {
    if (!g_ready) {
      ok = false;
      char b[96];
      snprintf(b, sizeof b, "not ready (frame %llu, GameProperties %p)",
               (unsigned long long)g_frame, (void*)props());
      return json_str(b);
    }
    if (!a.empty() && a[0] == "engine") {
      // Engine-side state the game's apply functions derive from the settings: evidence that a
      // change went through the game's own apply path, not just into GameProperties.
      auto* snd = (g_checks & C_APPLY) ? g<uint8_t*>(0xe13d50) : nullptr;  // sound manager
      char b[512];
      snprintf(b, sizeof b,
               "{\"snd.music\":%g,\"snd.effects\":%g,\"snd.speech\":%g,"
               "\"brightness.applied\":%g,\"brightness.exposure\":%g,"
               "\"render.motion_blur\":%d,\"render.film_grain\":%d,\"render.taa\":%d,"
               "\"render.lod_distance\":%g,\"frame\":%llu,\"presents\":%llu,\"ms\":%llu}",
               snd ? *reinterpret_cast<float*>(snd + 0x94) : -1.0,
               snd ? *reinterpret_cast<float*>(snd + 0x8c) : -1.0,
               snd ? *reinterpret_cast<float*>(snd + 0x90) : -1.0, g<float>(0x92d1c4),
               g<float>(0x955474), g<uint8_t>(0x95565b), g<uint8_t>(0x955667), g<uint8_t>(kTaa2),
               g<float>(0x955698), (unsigned long long)g_frame,
               (unsigned long long)present_frame(), (unsigned long long)GetTickCount64());
      return b;
    }
    if (!a.empty()) {
      Setting* s = find(a[0].find('.') == std::string::npos ? "game." + a[0] : a[0]);
      if (!s || !usable(*s)) {
        ok = false;
        return json_str("unknown or disabled setting");
      }
      return json_str(s->get());
    }
    std::string out = "{";
    for (auto& s : g_settings) {
      if (!usable(s)) continue;
      if (out.size() > 1) out += ",";
      out += json_str(s.id) + ":" + json_str(s.get());
    }
    return out + "}";
  });
}

void settings_start() {
  run_checks();
  prepatch_resolution_xml();
  if (!(g_checks & C_TICK) || !(g_checks & C_PROPS)) {
    TB_LOG("settings: off (no main-thread tick or no GameProperties)");
    return;
  }
  int n = 0;
  for (auto& s : g_settings)
    if (usable(s)) n++;
  TB_LOG("settings: %d of %zu settings usable", n, g_settings.size());
  for (auto& s : g_settings)
    if (usable(s)) quiver_live(s.id.c_str());  // Quiver sends these live over the SDK
  hook_fn(reinterpret_cast<void*>(at(kTimerTick)), reinterpret_cast<void*>(&tick_detour),
          reinterpret_cast<void**>(&g_tick_orig), "GameTimer::update");
}

}  // namespace tb
