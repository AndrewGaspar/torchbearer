# The game's own settings, live

Every option of AWR's options menu (graphics, display, audio, subtitles, controls, gamepad, key
bindings) is a Quiver option on surface `torchbearer` (`dist/quiver-options.toml`), ids `game.*`
and `keys.*`, implemented in `src/features/settings*.cpp`. All of them apply **live**: the mod
writes the value where the game keeps it and runs the game's own apply code, on the game's main
thread, then saves through the game's own savers so the game's files agree.

Addresses are RVAs in `Game_f_x64_EOS.exe` (Epic build, PE timestamp `0x697455b7`, SizeOfImage
`0xf65000`; image base `0x140000000`). Names are ours. "Static" = established in Ghidra
(`re/ghidra`, scripts in `re/scripts/`); "live" = observed in the lab (`awr-set`, runs r2..r8,
2026-11-18), with the evidence given.

## How it works

- **Where the settings live** (static). `GameProperties` is a singleton (`*0xb06ba0`, getter
  `0x1da6b0`). The menu's items are defined in `data\globaldb\menu.xml` (inside
  `data/ep999-000.rmdp`, plain text) plus three items the code adds (`hudsize`, `moderncamera`,
  `hdr`). `MenuMode::menuToSettings` (`0x25b100`) copies every item into GameProperties and the
  renderer globals and then calls `GameProperties::apply` (`0x1dd930`); `MenuMode::settingsToMenu`
  (`0x25c4e0`) goes the other way when a menu opens. `MenuMode::onItemChanged` (`0x24f130`) runs
  `menuToSettings` on every change, so the game itself applies everything immediately; only the
  resolution/window mode goes through a confirm dialog.
- **Applying** (static, then live). On a config change the value is queued; a hook on
  `GameTimer::update` (`0x1df8d0`, called once a frame from the main loop `0x263600`; live: it runs
  on the same thread as Present, and not during the startup videos) writes the queued values and
  calls, as needed: `GameProperties::apply(props, 0)` (`0x1dd930`: FMOD bus volumes, input
  inversion and sensitivity, gamepad scheme and vibration, brightness curve, anisotropic sampler,
  direct aiming, motion blur and film grain flags), `MenuMode::applyGraphicsQuality(menu, q)`
  (`0x242a20`; for presets 0..2 it never touches `menu`, so we pass null; for Custom we write the
  same globals it writes), the render device's DLSS reconfigure (`(*0xda35e0)->vtbl[0x368/8]`, as
  `menuToSettings` does), `Game::setDisplay(game, w, h, mode, dlss, display, hdr)` (`0x1d0e40`,
  what the menu's Apply does) and `Input::rebind(&name, vk)` (`0x3fe680`, what the keybind menu
  does; it unbinds other actions on that key). 30 frames after the last change it calls the
  game's own savers: `GameProperties::save` (`0x1dca80`, file `config`),
  `Game::saveResolutionXml(game, 0)` (`0x1d0b10`) and `Input::saveKeybinds` (`0x3fe920`).
- **An open options menu.** `menuToSettings` writes *every* item back, so a menu that was open
  while Quiver changed something would revert it on its next change. After each batch the mod
  calls `settingsToMenu(*0xb14400)` (the game's MenuMode, getter `0x24d5f0`), as the game does when
  a menu opens. Live (r5): with Graphics open, Quiver set VSync on and brightness 80; the open menu
  showed "On"; toggling Motion Blur in that menu kept VSync and brightness.
- **Startup.** The config is read before the game reads its files (we are a static dxgi import).
  Display values are written into `resolution.xml` right then, so the game starts in that mode
  (live r7: `resolution = 1600x900`, `display_mode = windowed` -> the first swapchain was
  1600x900, windowed). Everything else is applied on the 30th tick, once GameProperties exists
  (live r6: `settings: game.music 40 -> 25` etc. at "ready"). If the resolution.xml race were lost,
  the same values are applied live then.
- **No value = hands off.** The shipped ini has empty `[game]` and `[keys]` sections. With no value
  the mod never writes (live r8: no `settings:` writes and no saves with the shipped ini).
- **Reporting back.** Every 30 frames the mod reads all settings; a change it didn't make (the
  game's menu, the game's "Reverse your controls?" prompt) is logged, put into the config
  (`cfg_set`) and sent with `quiver_report(id, value, persist=true)`. At ready the game's current
  value of every setting the profile leaves alone is sent with `persist=false` (shown, not saved).
  Live: r3 subtitles and font size changed in the game's menu -> `game changed game.subtitles -> 1`;
  r7 answering Yes to the tutorial's invert prompt -> `game changed game.invert_y -> 1` and
  `cfg game.invert_y` = 1.
- **Signatures.** At start the mod checks the prologue bytes of every function it calls, that the
  getters load the expected globals, that the saver references the renderer globals in the
  expected order (which pins all of them), that the quality function references the custom
  globals, and so on (`run_checks`, 11 checks). A failed check disables only the settings that
  need it; without the tick or GameProperties checks the feature is off. All 11 pass on this
  build (every run's log).
- **Lab verbs.** `quiver lab call <i> game [id]` reads the live value(s) from the game's memory;
  `quiver lab call <i> game engine` reads state the apply functions derive (sound manager bus
  volumes, applied brightness and exposure, render flags, LOD distance, present count and clock).

## Settings

Ranges are the menu's (sliders are floats 0..1 in steps of 0.05, shown as 0..100 %). "File" is
the offset in `config` (format version `0x9f`, decoder: `re/scripts/awr_config.py`); offsets
after `0xbe` follow five variable-length unlock sets and are given for empty sets. `GP+x` is a
GameProperties field, `g:x` a global (RVA).

| id | type / range | memory | file | applied by | status, evidence |
|---|---|---|---|---|---|
| `game.resolution` | `WxH` | Game `+0xd4`/`+0xd8` | resolution.xml | setDisplay | live: ResizeBuffers 1280x720, 1600x900 in our log; screenshot 1280x720; resolution.xml rewritten |
| `game.display_mode` | fullscreen/windowed/borderless | Game `+0xdc` | resolution.xml | setDisplay | live: each switch logged ResizeBuffers; resolution.xml `window_mode` 1/2/0. Borderless uses the desktop resolution |
| `game.monitor` | display index | Game `+0xe0` | resolution.xml (name) | setDisplay | same path; **not verified** (one display in the lab) |
| `game.hdr` | bool | Game `+0xe4` | resolution.xml | setDisplay | same path; **not verified** (no HDR display in the lab; the menu greys it out) |
| `game.vsync` | bool | g:`0x95566c` | `0x04` | read per frame | live: 239 fps -> 60.1 fps -> 239 fps (`fps.sh`, presents per second) |
| `game.dlss` | off, ultra_performance, performance, balanced, quality, dlaa (0..5) | g:`0xd75620` | `0x6a`, resolution.xml | device reconfigure + quality | live: TAA/MSAA forced off as in the menu (`render.taa` 0), image visibly softer at Ultra Performance; that DLSS itself renders is **not proven** (no NGX log line) |
| `game.resolution_scale` | 50..100 step 10 | g:`0x9556b8` (0.5..1.0) | `0x72` | read per frame | live: 50 % visibly softer than 100 % (crop compare). Only with TAA and DLSS off |
| `game.brightness` | 0..100 | GP`+0x104` | `0x7a` | apply | live: frame mean 58.2 / 72.0 / 88.4 at 0 / 50 / 100; `brightness.exposure` 1 / 2.54 / 5 |
| `game.fov` | 75..125 % of default | GP`+0x204` (slider 0..1) | `0xc7` | camera update `0x1d3200` (tan(FOV/2) x (0.75 + slider/2)) | live in world: 75 vs 125 screenshots, clearly narrower/wider |
| `game.motion_blur` | bool | GP`+0x209` -> g:`0x95565b` | `0xcc` | apply | live: `render.motion_blur` follows, also from the game's menu |
| `game.film_grain` | bool | GP`+0x20a` -> g:`0x955667` | `0xcd` | apply | live: `render.film_grain` follows |
| `game.hud` | bool | GP`+0x114` | `0x08` | read by HUD drawing (`0x209277`, `0x214df4`, `0x2157ef`) | live by static reading; **not seen** (no HUD element on screen in the prologue; tutorial hints are not HUD) |
| `game.hud_size` | 0..100 (scale 1 + size) | GP`+0x118` | `0xe4` | read by HUD drawing (`0x2085d7`) | as `hud`. Game bug: its menu never loads this slider and writes 50 % back on any menu change (we report that) |
| `game.quality` | low/medium/high/custom | GP`+0x200` | `0xbb` | applyGraphicsQuality | live: presets switch all custom values (read back), volumetric light visibly changes (crop compare) |
| `game.taa` | bool | g:`0x955694` + g:`0x955636` | `0x6e` | quality (custom) | live: read back; resolution scale only takes effect with it on |
| `game.fxaa` | bool | g:`0xd755d9` | `0x22` | quality (custom) | live: read back, saved |
| `game.msaa` | bool | g:`0xd755d8` | `0x19` | quality (custom) | live: read back, saved |
| `game.anisotropy` | off/2x/4x/8x/16x | g:`0xd75618` (0/2/4/8/16) | `0x1e` | apply (sampler set) | live: read back, saved; visual not compared |
| `game.shadows` | low/medium/high | g:`0x955670` | `0x0d` | quality (custom) | live: read back, saved; renderer tweakable "Shadow Quality" |
| `game.volumetrics` | low/medium/high | g:`0x955688` | `0x29` | quality (custom) | live: read back; visible in the low/high preset comparison |
| `game.terrain` | low/medium/high | g:`0xd7561c` | `0x66` | quality (custom) | live: read back, saved |
| `game.draw_distance` | 0..100 | GP`+0x1fc` | `0xb7` | camera update `0x1d3200` (far plane x (0.75 + slider/4): 750..1000, or 1125..1500 where the game uses its long range) | live by static reading (same per-frame function as FOV, which was seen live); visual not compared |
| `game.music` | 0..100 | GP`+0x11c` | `0x87` | apply -> sound manager `+0x94` | live: `snd.music` 1 -> 0.4 |
| `game.effects` | 0..100 | GP`+0x120` | `0x8b` | apply -> sound manager `+0x8c` | live: `snd.effects` 1 -> 0.9 |
| `game.speech` | 0..100 | GP`+0x124` | `0x8f` | apply -> sound manager `+0x90` | live: `snd.speech` 1 -> 0.7 |
| `game.cinematic` | 0..100 | GP`+0x128` | `0x93` | apply -> `0x337050` | live: read back and saved; engine side not read |
| `game.english_speech` | bool | Locale `+0x24` (`0x3e5690`/`0x3e5320`) | `0x31` | read when lines play (`0x2db400`, `0x2db760`, `0x3387b0`, `0x339280`) | read back and saved; audible effect **not verified** |
| `game.subtitles` | bool | GP`+0x108` | `0x7e` | read by the subtitle renderer | live: subtitles shown in the intro; read back |
| `game.subtitle_size` | 0..100 | GP`+0x10c` | `0x7f` | subtitle renderer | read back, saved; visual change not caught (the line ended) |
| `game.subtitle_background` | 0..100 | GP`+0x110` | `0x83` | subtitle renderer | as above |
| `game.commentary` | off/original/remaster | GP`+0x140` | `0x76` | commentary player | live in world: the commentary portrait appeared with `remaster` and went away with `off` |
| `game.invert_y` | bool | GP`+0x12c` | `0x97` | apply (`0x3ff1f0`) | live in world: the same mouse move looked up, then down after the change |
| `game.invert_x` | bool | GP`+0x12d` | `0x98` | apply (`0x3ff1e0`) | read back, saved |
| `game.mouse_sensitivity_x` / `_y` | 0..100 | GP`+0x1f0` / `+0x1f4` | `0xad` / `0xb1` | apply (`0x3ff670`) | read back, saved |
| `game.direct_aiming` | bool | GP`+0x208` | `0xce` | apply (`0x1d2b20`) | read back, saved |
| `game.modern_camera` | off/recoil/camera_and_recoil | GP`+0x144` | `0xe0` | apply / camera | read back, saved |
| `game.gamepad_sensitivity` | 0..100 | GP`+0x130` | `0x99` | read by the input code (`0x2f5c0b`, `0x325ee4`) | read back, saved (menu: consoles only) |
| `game.vibration` | bool | GP`+0x134` | `0x9d` | apply (`0x3ff040`) | read back, saved |
| `game.gamepad_layout`, `game.driving_layout` | 1..2, 1..4 | GP`+0x138` = foot + 2 x drive | `0x9e` | apply (`0x3fec00`) | read back (5 = layout 2 + driving 3), saved |
| `game.swap_sticks`, `game.swap_triggers` | bool | GP`+0x13c`, `+0x13d` | `0xd6`, `0xd7` | apply (`0x3fec00`) | read back, saved |
| `keys.<action>` (21) | key name | `std::map<action,vk>` at `*0xd34230` | keybinds.xml | rebind `0x3fe680` + save `0x3fe920` | live in world: forward bound to I, W no longer walked, I walked; keybinds.xml rewritten |

All 62 testable settings were set at once through `torchbearer.ini` and read back from the game's
memory as set (r8, `roundtrip.py`, 62/62), then restored.

Not exposed: the menu's "Reset to defaults" (an action, not a setting), Display/Resolution as
menu list indices (we take the values instead), `graphicsqualityconsoles` and
`triggerhaptics` (consoles only), and the hidden `colorprofile` (developer).

### Key bindings

Names are Quiver's (`KeyW`, `Digit1`, `Space`, `Shift`, `Ctrl`, `Tab`, `F5`, `ArrowUp`,
`Numpad8`), plus `Mouse1`..`Mouse5` (left, right, middle, X1, X2) and raw `0x..` keybinds.xml
codes. keybinds.xml stores `key` = the non-extended scan code (`MapVirtualKeyA(vk, 0)`) and, for
numpad keys, `vk`; mouse buttons are `0x100` left, `0x101` middle, `0x102` right, `0x103`/`0x104`
X1/X2 (static, `0x3fcdd0`/`0x3fe920`). The action ids that can't be rebound in the menu
(`MENU_*`, `TOGGLE_MENU`, `ABORT_VIDEO_CONFIRM`) are not exposed.

### `config` layout (version 0x9f)

Written by `GameProperties::save` (`0x1dca80`) through a buffered file with typed writers
(vtable `+0xd0` int32, `+0xc0` float, `+0x100` bool, `+0xa8` string = u32 length + bytes,
`+0xf0` u8, `+0xd8` u16, `+0x88` 8 bytes); the GameProperties sub-object at `+0xb0` is written by
`0x1dcf20` and read by `0x1db130` (which also handles older versions). Then the collectibles block
(`0x117910`): names like `CANPYRAMIDS`, `THERMOS`. Confirmed by diffing the file after changes made
in the game's own menu (r3: subtitles `0x7e`, subtitle size `0x7f`, HUD size `0xe4`) and through
the mod (r4..r5: every field in the table).

## Notes and quirks

- DLSS replaces TAA and MSAA (the game turns them off, so do we); resolution scale is ignored
  while DLSS is on or TAA is off. These interplays come from `0x24f130`/`0x25ad70`/`0x242a20`.
- With a preset (not Custom) the individual quality values are the preset's; Quiver hides them.
- `hud_size`: see the table (game bug, reported back when it happens).
- Rebinding a key used by another action unbinds that action (the game's own behaviour).
- In the lab the config core keeps a value the game reported until the file changes that key
  (core semantics); under Quiver the report is saved to the profile, so both agree.
- `--mod` launches copy `dist/torchbearer.ini` over the instance's, so test values set with
  `quiver lab set` don't survive a relaunch with `--mod`.

## Verifying (lab)

```sh
quiver lab set awr-set 'install:torchbearer.ini:[game]brightness=80' --wait   # needs reload_pattern
quiver lab call awr-set game brightness        # the game's value, from its memory
quiver lab call awr-set game engine            # derived engine state (volumes, exposure, flags)
python3 -I re/scripts/awr_config.py config-before config-after   # what the game saved
```

Set `quiver lab edit awr-set 'reload_pattern=config reloaded' 'log_file=lab:runs/rN/torchbearer.log'`
for `--wait` (the run folder changes every launch).
