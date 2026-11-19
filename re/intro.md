# Startup sequence (intro logos / videos)

Established statically (Ghidra, `re/scripts/XrefDecomp.java` on the strings) and verified live in
lab instance `awr-intro` (2026-11-18).

`Game::Game` (`FUN_1401ce4c0`, it logs "Wanted resolution" / "Intro video performance") plays a
data-driven boot sequence while it streams the game in (`StreamJob_GameConstruct`):

| VA | What |
|---|---|
| `0x14092ceb8` | `const char* videos[]` = { `"startup_remedy.bik"` } (5.03 s) |
| `0x14092cec0` | `int video_count` = 1 |
| `0x14092cec4` | `int screen_count` = 3 |
| `0x14092cec8` | `IntroScreen* screens[3]` (read by `FUN_1401d06d0`, `IntroScreenPlayer`) |

`IntroScreen` (0x20 bytes): `{ float seconds; float fade; const char* text_key; float ?; u32 pad;
const char* texture; }`:

0. 3.0 s, fade 0.5, text `""`, `data\textures\menu\epic_games_boot.tex`
1. 3.0 s, fade 0.5, `MENU_RIGHTS_RESERVED`, `data\textures\menu\partner_logo_screen.tex` (d3t, NVIDIA, FMOD, Havok, Umbra)
2. 6.0 s, fade 0.5, `MENU_EPILEPSY_WARNING`, no texture

All of it is in `.data`, so `src/features/intro.cpp` rewrites the counts and compacts the array
from `DllMain` (before WinMain reaches `Game::Game`).

**Zero screens makes the game exit** about 17 s after the first present, cleanly, with no dump and before
EOS starts (bisected live: video-only, logos-only and video+logos skips all boot; skipping all
three screens exits). The screen player is what hands over to the menu, so "skip everything"
keeps one blank screen (`seconds = 0.01`, no text, no texture).

Measured (shots every ~2 s from the launch command): vanilla shows the title at ~29 s, full skip at
~19 s. The game uses the intro to hide its own loading, so the ~17 s of logos saves ~10 s.

Not part of this sequence: the title screen (`alan_wake_logo.bik`, menu mode 3, "Press any key to
play") and the idle attract loop (`startup_attract.bik`, mode 2) are played by `MenuMode`
(`FUN_140258480` → `FUN_140250be0`). `startup_mgs.bik` and `startup_epic.bik` ship but aren't in
the boot table. The 1.33 patch's "command to skip the intro" (for speedrunners) is probably the
in-game prologue, not this; not investigated.
