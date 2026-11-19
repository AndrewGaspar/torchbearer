# Getting to any level (testing)

There are no saves to start from, so Torchbearer loads levels itself (`src/features/levels.cpp`,
reverse-engineering notes in `re/levels.md`). Everything below was run live in the lab instance
`awr-lvl` (Nov 2026 build, 1920x1080, GPU shared with two other AWR instances).

## Quick use

```sh
export QUIVER_SOURCE=agent:<you>
quiver lab launch <i> --mod build/mod --wait menu    # or any state; the title screen is fine
quiver lab call <i> level 15                         # [easy|normal|nightmare], default easy
quiver lab wait <i> in-world --timeout 300
quiver lab call <i> where                            # {"x","y","z","yaw","world","camera",…}
quiver lab call <i> teleport 1390 47 2265 90         # x y z [yaw°]; y snaps to the ground
quiver lab call <i> level 04                         # switch level from in-world (5–20 s)
quiver lab call <i> level                            # list of ids
quiver lab call <i> skip                             # skip the Bink cutscene playing now
quiver lab call <i> game-state                       # raw mode/menu/loading/camera values
```

`level` works from the title screen ("Press any key"), the main menu, or in-world. From the menu
it deletes the checkpoint save in the slot first (the game would stop on its "Overwrite?" screen);
install a fixture again if you need it back.

Boot straight into a level with no lab calls at all (`torchbearer.ini`, read at launch):

```ini
[dev]
start_level = 15          ; or continue (load the checkpoint save, e.g. an installed fixture)
start_difficulty = easy
skip_videos = 1           ; skip Bink cutscenes as the player's skip would
developer_menu = 0        ; 1 = the game's hidden Developer Menu (unlock episodes, guns, flashlight)
```

With `start_level` and `skip_videos = 1` the game is **in-world about 13 s after launch**
(measured: `quiver lab launch … --wait in-world` → "in-world after 12.8s", level 03; 15.4 s for
`continue` from a fixture). Without `skip_videos`, level intros play their cutscene first
(level 02: 65 s of video).

Mind the argument order: `quiver lab call <i> level 02 --source x` passes `--source` to the mod as
an argument; put `--source` before the instance or use `QUIVER_SOURCE`.

### Lab states

The mod writes `%QUIVER_LAB%\state`: `boot` → `menu` (title/main menu) → `loading` → `video`
(a Bink cutscene) / `in-game` (gameplay mode but no control yet: in-engine cinematic camera) →
`in-world` (gameplay mode, nothing loading, no video, the player camera active, for 60 frames).
`paused` is the in-game pause menu. Every state line also carries `world=<world:level>` (empty
after `continue`, which loads through the checkpoint path).

## Level list

The game's own ids (from `data/database/menu.xml`, see `re/levels.md`). "Start" is where `level
<id>` puts you once in-world (`where` output, easy difficulty, videos skipped); every one was
loaded live, each in-world within 4.5–5.8 s of the switch from another level (survey of
`level <id>` from in-world, one after another, screenshots checked).

| id | world:level | episode | internal name | start: what you see | start x, y, z, yaw |
|---|---|---|---|---|---|
| 01 | gameworld:level_01 | 1 Nightmare | Intro | night, nightmare road through the forest, street lights, lighthouse beam (the prologue) | -3104.5, 84.9, -932.1, -120 |
| 02 | gameworld:level_02 | 1 Nightmare | Arrival | **day**, on the ferry facing Bright Falls (bridge, town, lake) | 16.9, 2.2, -190.3, -12.5 |
| 03 | gameworld:level_03 | 1 Nightmare | Meeting Stucky | night, forest, by the crashed car under a rock face | -26.4, 128.1, -1445.0, -16 |
| 04 | gameworld:level_04 | 2 Taken | Sheriff Station | indoor, the New York apartment dream (lamp-lit rooms, doorways) | -4138.8, 98.1, -4159.0, -9 |
| 05 | gameworld:level_05 | 2 Taken | Elderwood | indoor, day, Elderwood ranger station lodge with Rusty | -1295.8, 61.9, -3251.1, -140 |
| 09 | gameworld:level_09 | 3 Ransom | Trailer Park | day, trailer park with the manager | -44.7, 61.9, -956.3, -99.5 |
| 10 | gameworld:level_10 | 3 Ransom | Mirror Peak | day, forest overlook, "Welcome to Bright Falls" billboard | 730.6, 208.7, -2339.7, 0.5 |
| 11 | gameworld:level_11 | 4 The Truth | Cauldron Lake Lodge | indoor, bright cabin room with typewriter (dream) | 981.6, 229.3, -1160.4, 105 |
| 12 | gameworld:level_12 | 4 The Truth | Andersson Farm | night, dense forest on rocks, grey | -1772.3, 118.1, 1997.4, -97.5 |
| 13 | gameworld:level_13 | 4 The Truth | Cabin Dream | night, Diver's Isle gate, bright light | 1386.0, 82.2, -1802.0, -32.5 |
| 14 | gameworld:level_14 | 5 The Clicker | Bright Falls | indoor, sheriff station cell with Barry | 31.3, 5.6, 253.1, -3.65 |
| 15 | gameworld:level_15 | 5 The Clicker | Dam | night, fog, lumber yard at the forest edge, flashlight on | 1385.4, 46.4, 2261.8, 122.5 |
| 16 | gameworld:level_16 | 6 Departure | Final Fight | indoor, dark apartment by the window (dream) | -4126.8, 98.1, -4148.5, 0 |
| 17 | gameworld:level_17 | 6 Departure | End | indoor, green-lit apartment bedroom with Alice | -4129.1, 98.1, -4151.3, 32.5 |
| 100 | dlc1_world:level_100 | Special 1 The Signal | The Signal | indoor, night, Oh Deer Diner (neon) | 90.2, 3.5, 215.6, -22.5 |
| 200 | dlc2_world:level_200 | Special 2 The Writer | The Writer | night, fog, iron cemetery gate with Barry | 915.6, 221.7, -1079.9, 141 |

There are no levels 06–08 in the data. Aliases: `5`, `05`, `level_05`, `gameworld:level_05`;
`dlc1`/`signal` = 100, `dlc2`/`writer` = 200; anything containing `:` is passed to the game as is.
Checkpoints inside a level are not addressable: each level loads at its start (the game writes a
checkpoint save there, "Checkpoint reached"), and later spots are reached with `teleport` (the
level's scripting does not follow you, so story triggers between are skipped) or by playing to a
checkpoint and saving a fixture.

Coordinates: Y is up, yaw 0 faces +Z and 90 faces +X (established live: the camera sits behind the
player on the opposite side). `teleport` keeps the heading when yaw is omitted.

## Fixtures

Saved with `quiver lab fixture save awr-lvl …` right after `level <id>` reached in-world (the
level-start checkpoint is written during the load). Restore: `quiver lab fixture install <i>
<name>` (game stopped), then launch with `[dev] start_level = continue` (or `quiver lab call <i>
level continue` at the menu). Verified: `awr-forest-night` installed into a fresh launch with
`start_level = continue` → in-world after 15.4 s at the saved spot.

`fixture install` copies the save **read-only**; the game then cannot overwrite or delete its
checkpoint (found live: the menu stopped on "Existing checkpoint will be overwritten"). The mod
clears the read-only attribute on `savegame_aw1\*` before `level` / `continue` touch the slot.
Without the mod (plain Continue in the menu) later checkpoints of that run may fail to save.

| fixture | level | scene |
|---|---|---|
| `awr-forest-night` | 15 (Dam) | night, fog, forest edge and lumber yard, flashlight lit: outdoor with depth |
| `awr-apartment-indoor` | 04 (Sheriff Station) | indoor, lamp-lit apartment rooms seen through doorways |
| `awr-brightfalls-day` | 02 (Arrival) | daylight, ferry facing Bright Falls town, bridge and lake |

The existing `prologue-road` fixture (from the stereo work) is level 01's road; `level 01` with
`skip_videos = 1` lands at the same spot.

## How it works (short)

All actions run on the game thread from a hook on `ModeSwitch::update` (0x140263600), the
per-frame mode manager:

- From the title it does what a key press does (`FUN_14024d200`: load profile, main menu), then
  calls the main menu's own `load_episode <world:level>` action (`MenuMode::action`
  0x14024d8f0) after setting the difficulty and deleting any checkpoint save in the slot (the
  game would otherwise block on an "overwrite?" platform dialog). `continue` calls the menu's
  Continue (0x140257920).
- In-world it writes the world name into `GameProperties`, which `ModeSwitch::update` hands to
  `LoadingMode::load` on its next frame (the debug panel's "Load World" path).
- `teleport` calls `Character` set-transform (0x14017df50) on the player, what the character
  script's `Teleport` action uses.
- `skip_videos` stops the Bink video the way the player's skip does (0x140337090 + finished flag).

## What didn't work / not done

- The old `-developermenu` command line is gone from the Remastered parser (it only reads the
  Epic/AUTH arguments); the flag byte is still there and `[dev] developer_menu` sets it.
- The debug panel's own teleport / load-state / "Load Checkpoint FROM DISK" entries exist but their
  handlers are stubbed in this build; the `TeleportPlayer` script function is a no-op.
- No synthesised saves: not needed (level select plus level-start fixtures cover it). Online saves
  were not used.
- Teleporting while the player is on a scripted platform (level 02's ferry) is allowed but
  untested; in-engine cinematics (camera mode 3) are not skipped, only Bink videos.
