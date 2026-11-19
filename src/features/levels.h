#pragma once

namespace tb {

// Level select and player position for testing (docs/levels.md, re/levels.md).
//  - `quiver lab call <i> level <id> [easy|normal|nightmare]`: load a level from the title, the
//    main menu or in-world (the world is switched), then `quiver lab wait <i> in-world`.
//    `level continue` loads the checkpoint save (e.g. an installed fixture); `level` lists ids.
//  - `[dev] start_level = <id>|continue`: boot straight into it, past the title screen.
//  - `where` / `teleport x y z [yaw]`: the player's position (world units, Y up) and heading.
//  - `skip` / `[dev] skip_videos = 1`: skip Bink cutscenes as the player's skip does.
//  - `[dev] developer_menu = 1`: the game's own (hidden) Developer Menu in the main/pause menu.
//  - `game-state`: the raw mode/menu/loading/camera picture, for debugging.
// Lab state: `boot`, `menu`, `loading`, `video`, `in-game` (no control yet), `paused`,
// `in-world` (player camera, nothing loading, no video, for a second).
void levels_register();
void levels_start();

}  // namespace tb
