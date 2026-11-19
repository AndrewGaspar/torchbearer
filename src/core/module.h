#pragma once
#include <cstdint>
#include <string>

namespace tb {

// The game's install folder (where Game_f_x64_EOS.exe and our dxgi.dll live), no trailing slash.
std::wstring game_dir();

// The main executable's image: base and size, for RVA <-> address and range checks.
struct Image {
  uintptr_t base = 0;
  size_t size = 0;
  uintptr_t at(uintptr_t rva) const { return base + rva; }
  bool contains(uintptr_t p) const { return p >= base && p < base + size; }
};
const Image& game_image();

}  // namespace tb
