// Skip the startup sequence: the Remedy video and the three intro screens (Epic boot logo, partner
// logos + rights notice, epilepsy warning).
//
// Game::Game (FUN_1401ce4c0) plays a data-driven sequence before the main menu:
//   0x14092ceb8  const char* videos[]   = { "startup_remedy.bik" }
//   0x14092cec0  int video_count        = 1
//   0x14092cec4  int screen_count       = 3
//   0x14092cec8  IntroScreen* screens[] = { epic boot (3 s), partner logos + MENU_RIGHTS_RESERVED
//                                           (3 s), MENU_EPILEPSY_WARNING (6 s) }
// IntroScreen: { float seconds; float fade; const char* text_key; float ?; const char* texture }.
// All of it is in .data, so we just rewrite the counts / compact the pointer array. This runs
// from DllMain: Game::Game runs early in WinMain, and our init thread could lose that race.
// (Established statically in Ghidra: re/intro.md.)
#include "features/intro.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "core/log.h"
#include "core/module.h"

namespace tb {
namespace {

constexpr uintptr_t kVideos = 0x92ceb8;
constexpr uintptr_t kVideoCount = 0x92cec0;
constexpr uintptr_t kScreenCount = 0x92cec4;
constexpr uintptr_t kScreens = 0x92cec8;

struct IntroScreen {
  float seconds;
  float fade;
  const char* text_key;
  float unknown;
  uint32_t pad;
  const char* texture;
};

bool str_at(const Image& img, const char* p, const char* want) {
  auto u = reinterpret_cast<uintptr_t>(p);
  return p && img.contains(u) && std::strcmp(p, want) == 0;
}

int ini_int(const wchar_t* key, int def) {
  std::wstring path = game_dir() + L"\\torchbearer.ini";
  return int(GetPrivateProfileIntW(L"intro", key, def, path.c_str()));
}

// What was done, for the log once it exists (DllMain can't log: the log opens later).
std::string g_summary = "not run";

}  // namespace

void intro_early() {
  const Image& img = game_image();
  auto* videos = reinterpret_cast<const char**>(img.at(kVideos));
  auto* video_count = reinterpret_cast<int*>(img.at(kVideoCount));
  auto* screen_count = reinterpret_cast<int*>(img.at(kScreenCount));
  auto* screens = reinterpret_cast<IntroScreen**>(img.at(kScreens));

  // Signature: the exact tables of this build.
  bool ok = *video_count == 1 && str_at(img, videos[0], "startup_remedy.bik") && *screen_count == 3;
  if (ok) {
    for (int i = 0; i < 3 && ok; i++) ok = img.contains(reinterpret_cast<uintptr_t>(screens[i]));
  }
  if (ok) {
    ok = str_at(img, screens[0]->text_key, "") &&
         str_at(img, screens[0]->texture, "data\\textures\\menu\\epic_games_boot.tex") &&
         str_at(img, screens[1]->text_key, "MENU_RIGHTS_RESERVED") &&
         str_at(img, screens[1]->texture, "data\\textures\\menu\\partner_logo_screen.tex") &&
         str_at(img, screens[2]->text_key, "MENU_EPILEPSY_WARNING");
  }
  if (!ok) {
    g_summary = "intro tables don't match this build; left alone";
    return;
  }

  bool skip_video = ini_int(L"skip_startup_video", 1) != 0;
  bool skip_logos = ini_int(L"skip_logo_screens", 1) != 0;
  bool skip_warning = ini_int(L"skip_epilepsy_warning", 1) != 0;

  if (skip_video) *video_count = 0;
  IntroScreen* keep[3];
  int n = 0;
  if (!skip_logos) keep[n++] = screens[0], keep[n++] = screens[1];
  if (!skip_warning) keep[n++] = screens[2];
  // With zero screens the game exits a few seconds after boot (measured in the lab: every
  // combination keeping >= 1 screen boots; zero screens -> clean exit before EOS starts). The
  // screen player is what hands over to the menu, so keep one blank screen of ~one frame instead.
  if (n == 0) {
    IntroScreen* blank = screens[2];
    blank->seconds = 0.01f;
    blank->fade = 0.0f;
    blank->text_key = screens[0]->text_key;  // "" (the Epic boot screen has no text)
    blank->texture = nullptr;
    keep[n++] = blank;
  }
  for (int i = 0; i < n; i++) screens[i] = keep[i];
  *screen_count = n;

  g_summary = std::string("startup video ") + (skip_video ? "skipped" : "kept") + ", logo screens " +
              (skip_logos ? "skipped" : "kept") + ", epilepsy warning " +
              (skip_warning ? "skipped" : "kept");
}

void intro_report() { TB_LOG("intro: %s (applies at launch)", g_summary.c_str()); }

}  // namespace tb
