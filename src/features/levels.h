#pragma once

namespace tb {

// Level select and player position for testing (docs/levels.md, re/levels.md).
//  - `quiver lab call <i> level <id> [easy|normal|nightmare]`: load a level (from the main
//    menu, or in-world: the world is switched), then `quiver lab wait <i> in-world`.
//  - `[dev] start_level = <id>`: boot straight into that level once the main menu is up.
//  - `where` / `teleport x y z [yaw]`: the player's position (world units, Y up) and heading.
//  - `[dev] developer_menu = 1`: the game's own (hidden) Developer Menu in the main/pause menu.
// Lab state: `boot`, `menu`, `loading`, `in-world` (player exists and has control).
void levels_register();
void levels_start();

}  // namespace tb
