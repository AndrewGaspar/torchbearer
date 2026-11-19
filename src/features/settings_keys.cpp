// Key names for keybinds (docs/settings.md, "Keybinds").
//
// keybinds.xml stores `key` = the non-extended scan code (what MapVirtualKeyA(vk, MAPVK_VK_TO_VSC)
// returns), `key2` = the VK for numpad keys only (so Numpad8 and ArrowUp, both scan 0x48, differ),
// and 0x100..0x104 for the mouse buttons (left, middle, right, X1, X2). In memory the game keeps the VK (static, RVA 0x3fcdd0).
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <string>

#include "features/settings.h"

namespace tb {
namespace {

struct Named {
  const char* name;
  unsigned scan;
  unsigned vk;  // the VK the game ends up with (for reading back), and key2 for numpad keys
  bool key2;    // write vk as key2 (numpad keys)
};

// Names follow Quiver's normalisation (crates/quiver-core/src/options/keys.rs): KeyW, Digit1,
// F1, Space, Enter, Shift, Ctrl, Alt, ArrowUp, Numpad8, Backquote, ...
constexpr Named kKeys[] = {
    {"Escape", 0x01, VK_ESCAPE, false},   {"Digit1", 0x02, '1', false},
    {"Digit2", 0x03, '2', false},         {"Digit3", 0x04, '3', false},
    {"Digit4", 0x05, '4', false},         {"Digit5", 0x06, '5', false},
    {"Digit6", 0x07, '6', false},         {"Digit7", 0x08, '7', false},
    {"Digit8", 0x09, '8', false},         {"Digit9", 0x0a, '9', false},
    {"Digit0", 0x0b, '0', false},         {"Minus", 0x0c, VK_OEM_MINUS, false},
    {"Equal", 0x0d, VK_OEM_PLUS, false},  {"Backspace", 0x0e, VK_BACK, false},
    {"Tab", 0x0f, VK_TAB, false},         {"KeyQ", 0x10, 'Q', false},
    {"KeyW", 0x11, 'W', false},           {"KeyE", 0x12, 'E', false},
    {"KeyR", 0x13, 'R', false},           {"KeyT", 0x14, 'T', false},
    {"KeyY", 0x15, 'Y', false},           {"KeyU", 0x16, 'U', false},
    {"KeyI", 0x17, 'I', false},           {"KeyO", 0x18, 'O', false},
    {"KeyP", 0x19, 'P', false},           {"BracketLeft", 0x1a, VK_OEM_4, false},
    {"BracketRight", 0x1b, VK_OEM_6, false}, {"Enter", 0x1c, VK_RETURN, false},
    {"Ctrl", 0x1d, VK_CONTROL, false},    {"KeyA", 0x1e, 'A', false},
    {"KeyS", 0x1f, 'S', false},           {"KeyD", 0x20, 'D', false},
    {"KeyF", 0x21, 'F', false},           {"KeyG", 0x22, 'G', false},
    {"KeyH", 0x23, 'H', false},           {"KeyJ", 0x24, 'J', false},
    {"KeyK", 0x25, 'K', false},           {"KeyL", 0x26, 'L', false},
    {"Semicolon", 0x27, VK_OEM_1, false}, {"Quote", 0x28, VK_OEM_7, false},
    {"Backquote", 0x29, VK_OEM_3, false}, {"Shift", 0x2a, VK_SHIFT, false},
    {"Backslash", 0x2b, VK_OEM_5, false}, {"KeyZ", 0x2c, 'Z', false},
    {"KeyX", 0x2d, 'X', false},           {"KeyC", 0x2e, 'C', false},
    {"KeyV", 0x2f, 'V', false},           {"KeyB", 0x30, 'B', false},
    {"KeyN", 0x31, 'N', false},           {"KeyM", 0x32, 'M', false},
    {"Comma", 0x33, VK_OEM_COMMA, false}, {"Period", 0x34, VK_OEM_PERIOD, false},
    {"Slash", 0x35, VK_OEM_2, false},     {"ShiftRight", 0x36, VK_RSHIFT, false},
    {"NumpadMultiply", 0x37, VK_MULTIPLY, true}, {"Alt", 0x38, VK_MENU, false},
    {"Space", 0x39, VK_SPACE, false},     {"CapsLock", 0x3a, VK_CAPITAL, false},
    {"F1", 0x3b, VK_F1, false},           {"F2", 0x3c, VK_F2, false},
    {"F3", 0x3d, VK_F3, false},           {"F4", 0x3e, VK_F4, false},
    {"F5", 0x3f, VK_F5, false},           {"F6", 0x40, VK_F6, false},
    {"F7", 0x41, VK_F7, false},           {"F8", 0x42, VK_F8, false},
    {"F9", 0x43, VK_F9, false},           {"F10", 0x44, VK_F10, false},
    {"F11", 0x57, VK_F11, false},         {"F12", 0x58, VK_F12, false},
    {"Pause", 0x45, VK_PAUSE, false},     {"ScrollLock", 0x46, VK_SCROLL, false},
    {"Home", 0x47, VK_HOME, false},       {"ArrowUp", 0x48, VK_UP, false},
    {"PageUp", 0x49, VK_PRIOR, false},    {"ArrowLeft", 0x4b, VK_LEFT, false},
    {"ArrowRight", 0x4d, VK_RIGHT, false}, {"End", 0x4f, VK_END, false},
    {"ArrowDown", 0x50, VK_DOWN, false},  {"PageDown", 0x51, VK_NEXT, false},
    {"Insert", 0x52, VK_INSERT, false},   {"Delete", 0x53, VK_DELETE, false},
    {"Numpad7", 0x47, VK_NUMPAD7, true},  {"Numpad8", 0x48, VK_NUMPAD8, true},
    {"Numpad9", 0x49, VK_NUMPAD9, true},  {"NumpadSubtract", 0x4a, VK_SUBTRACT, true},
    {"Numpad4", 0x4b, VK_NUMPAD4, true},  {"Numpad5", 0x4c, VK_NUMPAD5, true},
    {"Numpad6", 0x4d, VK_NUMPAD6, true},  {"NumpadAdd", 0x4e, VK_ADD, true},
    {"Numpad1", 0x4f, VK_NUMPAD1, true},  {"Numpad2", 0x50, VK_NUMPAD2, true},
    {"Numpad3", 0x51, VK_NUMPAD3, true},  {"Numpad0", 0x52, VK_NUMPAD0, true},
    {"NumpadDecimal", 0x53, VK_DECIMAL, true}, {"NumpadDivide", 0x35, VK_DIVIDE, true},
    // Mouse buttons: the game's codes 0x100..0x104 are left, middle, right, X1, X2.
    {"Mouse1", 0x100, VK_LBUTTON, false}, {"Mouse2", 0x102, VK_RBUTTON, false},
    {"Mouse3", 0x101, VK_MBUTTON, false}, {"Mouse4", 0x103, VK_XBUTTON1, false},
    {"Mouse5", 0x104, VK_XBUTTON2, false},
};

// Other spellings people (or other tools) use.
struct Alias {
  const char* from;
  const char* to;
};
constexpr Alias kAliases[] = {
    {"SHIFTLEFT", "Shift"},  {"LSHIFT", "Shift"},       {"RSHIFT", "ShiftRight"},
    {"CONTROL", "Ctrl"},     {"CONTROLLEFT", "Ctrl"},   {"LCTRL", "Ctrl"},
    {"ALTLEFT", "Alt"},      {"LALT", "Alt"},           {"RETURN", "Enter"},
    {"ESC", "Escape"},       {"UP", "ArrowUp"},         {"DOWN", "ArrowDown"},
    {"LEFT", "ArrowLeft"},   {"RIGHT", "ArrowRight"},   {"LMB", "Mouse1"},
    {"RMB", "Mouse2"},       {"MMB", "Mouse3"},         {"MOUSELEFT", "Mouse1"},
    {"MOUSERIGHT", "Mouse2"}, {"MOUSEMIDDLE", "Mouse3"}, {"BACKSPACE", "Backspace"},
};

// The VK the game ends up with for a table entry, computed the way its loader does (RVA
// 0x3fcdd0): key2 if given, the mouse mapping, else MapVirtualKeyA(scan, MAPVK_VSC_TO_VK).
unsigned mem_vk(const Named& k) {
  if (k.key2 || k.scan >= 0x100) return k.vk;
  unsigned vk = MapVirtualKeyA(k.scan, MAPVK_VSC_TO_VK);
  return vk ? vk : k.vk;
}

std::string upper(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool key_from_name(const std::string& raw, GameKey* out) {
  std::string n = raw;
  while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back()))) n.pop_back();
  while (!n.empty() && std::isspace(static_cast<unsigned char>(n.front()))) n.erase(0, 1);
  if (n.empty()) return false;
  // Raw keybinds.xml code ("0x39").
  if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) {
    out->scan = unsigned(std::strtoul(n.c_str() + 2, nullptr, 16));
    out->vk = 0;
    // As the game's loader does (RVA 0x3fcdd0): mouse codes, else the scan code's VK.
    static const unsigned mouse[] = {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    out->vk_mem = out->scan >= 0x100 && out->scan <= 0x104 ? mouse[out->scan - 0x100]
                                                          : MapVirtualKeyA(out->scan, MAPVK_VSC_TO_VK);
    return out->scan != 0 && out->vk_mem != 0;
  }
  std::string u = upper(n);
  if (u.rfind("VK_", 0) == 0) u = u.substr(3);
  for (auto& a : kAliases)
    if (u == a.from) u = upper(a.to);
  // Bare letters and digits ("W", "1").
  if (u.size() == 1 && std::isalpha(static_cast<unsigned char>(u[0]))) u = "KEY" + u;
  if (u.size() == 1 && std::isdigit(static_cast<unsigned char>(u[0]))) u = "DIGIT" + u;
  for (auto& k : kKeys) {
    if (upper(k.name) == u) {
      out->scan = k.scan;
      out->vk = k.key2 ? k.vk : 0;
      out->vk_mem = mem_vk(k);
      return true;
    }
  }
  return false;
}

std::string key_to_name(unsigned vk) {
  for (auto& k : kKeys)
    if (mem_vk(k) == vk) return k.name;
  for (auto& k : kKeys)
    if (k.vk == vk) return k.name;
  if (vk == VK_LSHIFT) return "Shift";
  if (vk == VK_LCONTROL || vk == VK_RCONTROL) return "Ctrl";
  if (vk == VK_LMENU || vk == VK_RMENU) return "Alt";
  char buf[16];
  snprintf(buf, sizeof buf, "0x%x", MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
  return buf;
}

}  // namespace tb
