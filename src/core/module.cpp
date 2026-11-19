#include "core/module.h"

#include <windows.h>

namespace tb {

std::wstring game_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

const Image& game_image() {
  static Image img = [] {
    Image i;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    i.base = reinterpret_cast<uintptr_t>(base);
    i.size = nt->OptionalHeader.SizeOfImage;
    return i;
  }();
  return img;
}

}  // namespace tb
