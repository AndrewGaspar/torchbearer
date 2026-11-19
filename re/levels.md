# Levels, menus, player position (RE notes for `src/features/levels.cpp`)

Image base 0x140000000; addresses below are VAs, the code uses RVAs (VA − 0x140000000). Static
findings come from the Ghidra project (`re/scripts/Serve.java` query server, a copy of
`re/ghidra`); "live" means checked in the lab (`awr-lvl`, Nov 2026 build, exe size 0xf65000).

## Level ids (static: `data/database/menu.xml`, live: every id loaded)

`menu.xml` is stored uncompressed in `data/ep999-000.rmdp` (search for `<submenu name="main">`).
The episode menu's commands are `load_episode gameworld:level_NN` (episodes 1–6) and
`load_episode dlc1_world:level_100.lvese` / `dlc2_world:level_200.lvese` (the specials). The
`developer` submenu has a commented-out list with the internal level names (Intro, Arrival, …,
see `docs/levels.md`). The `.lvese` suffix is stripped by the loader (`FUN_14024ff00` cuts at the
first `.`). There are no levels 06–08: the numbering skips them.

`FUN_140250140` (the PS5 "activity" launcher) maps `ACT_NAME_LEVEL_<n>` to the same names:
n < 10 → `gameworld:level_0n`, < 100 → `gameworld:level_n`, < 200 → `dlc1_world:level_n`, else
`dlc2_world:level_n`.

## Starting a level

- `MenuMode::action(menu, String cmd, String arg)` = `FUN_14024d8f0`. Commands include
  `new_game` (→ `gameworld:level_01`), `load_episode <world:level>`, `load_checkpoint <i>`,
  `restart_checkpoint`, `difficulty easy|hard|nightmare`, and the developer-menu cheats
  `cheat_receive_weapons`, `cheat_receive_flashlight`, `cheat_unlock_levels`,
  `cheat_unlock_nightmare`.
- `load_episode`: if the save slot has no checkpoint (`SaveGameManager::state` `FUN_1402b9420`
  == 1) it selects slot (0,1) (`FUN_1402bb1e0`) and calls `FUN_14024ff00(menu, name)`; otherwise
  it goes to an overwrite-confirm screen. `FUN_14024ff00` checks platform sign-in/storage (blocking
  platform message boxes if not), asks `checkpoint_overwrite` (blocking) if a save exists, stops
  menu videos (`FUN_140336040`), writes the name into `GameProperties` (+0, a String; global via
  `FUN_1401da6b0`) and closes the menu (`FUN_140244f40`).
- `ModeSwitch::update` = `FUN_140263600` (ModeSwitch.cpp, the per-frame mode manager, called on
  the render/game thread): if `GameProperties.level` is non-empty it calls
  `LoadingMode::load(name)` (`FUN_140236ef0`) and clears it. The debug panel's "Load World"
  entries use the same `FUN_140236ef0`. So writing the String from the game thread switches world
  from anywhere in-world (verified live: level 03 → 04 → … in 5–20 s).
- Difficulty: `DifficultySystem*` (`FUN_1401b09d0` → `DAT_140b02990`), +0 int: 0 easy,
  1 normal ("hard" in the menu), 2 nightmare.
- Delete the slot's checkpoint: `FUN_1402b9240` (what "yes, overwrite" does).
- Continue (main menu "Continue Game", and `load_checkpoint`/`restart_checkpoint`):
  `FUN_140257920(menu)` reads and loads the slot's checkpoint.
- Saves (Epic PC): `Documents/Remedy/AlanWakeRemastered/savegames/savegame_aw1/savegame.aws` in the
  prefix (`%ssavegames\savegame_aw1\%s`). It holds the profile and, after the first "Checkpoint
  reached", the checkpoint (~145 bytes → ~170 KB). A level start writes a checkpoint, so a
  fixture saved right after `level <id>` restores that level start.

The shipped command-line parser (`FUN_140195cf0`) only knows the Epic/AUTH arguments; the old
`-developermenu` etc. are gone, but the option bytes still exist: `FUN_140195fc0` →
`DAT_140b01c88`, +0xc "developer", +0xd "developermenu" (the menu condition `developermenu` in
`FUN_1402439d0` reads +0xd). Setting +0xd shows `menu.xml`'s Developer Menu in the main and pause
menus (`[dev] developer_menu`).

## Menu screens (MenuMode, singleton `FUN_14024d5f0` → `DAT_140b14400`)

+0x14 current screen, +0x1c screen being switched to (0 = none). Live: 1 boot, then 3 title
("Press any key to play"), 2 attract video after 19 s idle on the title, 4 main menu, 5 pause.
In `MenuMode::update` (`FUN_140259560`) a key on the title (signed in) calls `FUN_14024d200`
(load profile, go to screen 4); we call the same.

## Modes (ModeSwitch, `this` of `FUN_140263600`)

+0xa0 the active mode, +0x98 the previous one, +0x58 VideoMode, +0x60 MenuMode. Mode vtables:
MenuMode 0x140764ac0, GameMode 0x14075d308, LoadingMode 0x1407634f8, VideoMode 0x1407726e8,
CreditsMode 0x140757fc0. Mode +0xc = active (`FUN_140261850`). LoadingMode (`FUN_140236ce0` →
`DAT_140b13be0`): +0x10 state (0 or 0xc idle, else loading), +0x48 String world:level.

## Cutscenes

- Bink cutscenes: VideoMode active (+0xc). The player's skip (`VideoMode::update`
  `FUN_140337140`, ABORT_VIDEO then confirm) stops the playing video with `FUN_140337090(video)`
  and sets +0x61a (finished); ModeSwitch then exits VideoMode. +0x610/+0x614 are the queue
  read/write indices, +0x618 "starting". `[dev] skip_videos` / the `skip` verb do the same.
- Camera mode (`FUN_1401d2540` → `DAT_140b05e28`, from `FUN_1401d2fe0`): 0 player camera,
  1/2/6 free camera, 3 cinematic (in-engine cutscenes), 4/5 animation preview, 7 death.

## Player

- Player manager `FUN_1401873a0` → `DAT_140b00020`; +0x50 the player `Character*`
  (`FUN_140184010`; the menu's `player_exists` condition).
- `Character` world transform at +0xbc (`FUN_1402652c0`): 3×3 rotation (row-major, 9 floats)
  then position at +0xe0 (x, y, z). Y is up; yaw 0 faces +Z, 90 faces +X (live: camera sits
  behind the player on the opposite side). Yaw = atan2(−m[2], m[0]).
- Teleport: `FUN_14017df50(character, float xform[12])` (rotation then position), what the
  character script's `Teleport x y z heading` action calls after
  `FUN_14014c4c0(rot, 0, heading_rad, 0, 0)` (euler → matrix). Live: works on foot; the game
  snaps y to the ground and the camera follows. Script `TeleportPlayer` is a no-op in this build.
- +0x1c9 is set on the player character (not "in vehicle"); the script Teleport refuses it, the
  setter itself handles it (resets the player camera).
- PlayerCamera `FUN_1401d2570` → `DAT_140b05df0`, position at +0xcec, rotation at +0xcc8.
- `EnablePlayerControls` flags (`FUN_1403ff690`): two bytes; both stay 1 through intro
  cinematics, so "has control" is taken as GameMode active + not loading + player + camera
  mode 0 + no video, for 60 frames.

## Unused leads

- `DebugPanel::MainPage` (`FUN_140239910`) registers "Drop player", "Teleport X/Y/Z", "Load
  World", "Load/Save State 0–2" and "Load Checkpoint FROM DISK", but its action slots are stubs
  (`FUN_14011baa0` returns 0) in this build.
- Script functions (`FUN_1401d6c00`): `DEBUG_UnlockLevel`, `DEBUG_UnlockAllLevels`,
  `DEBUG_SetTweakable`, `StartEpisode` (GameMode vtbl+0x40, `FUN_1401d5530`).
- The level part names `level_1_part_1` … `level_16_part_4` (`FUN_1400132c0`) look like
  achievement/telemetry segments, not loadable checkpoints.
