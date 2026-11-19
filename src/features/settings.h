#pragma once
#include <string>

namespace tb {

// The game's own settings (its options menu) exposed as config ids `game.*` and `keys.*`, applied
// live through the game's own code on its main thread (docs/settings.md).
void settings_register();  // config listener + lab verb `game`
void settings_start();     // signature checks + the main-thread tick hook

// Keybind names (W3C KeyboardEvent.code style, as Quiver normalizes them) <-> the game's key codes
// (a DirectInput scan code; 0x100..0x104 are mouse buttons; numpad keys carry their VK as well).
struct GameKey {
  unsigned scan = 0;    // value of keybinds.xml `key`
  unsigned vk = 0;      // value of keybinds.xml `key2` (0 = derive from the scan code)
  unsigned vk_mem = 0;  // the VK the game keeps in memory for the binding
};
bool key_from_name(const std::string& name, GameKey* out);
std::string key_to_name(unsigned vk);  // from the VK the game keeps in memory

}  // namespace tb
