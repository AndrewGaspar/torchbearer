# Stereo output: the 3840x1080 window

How full side-by-side output reaches a 3840x1080 display (XReal Air 2 Ultra in 3D mode) while
the engine renders one 1920x1080 eye. Code: `src/features/stereo/window.cpp`, `src/core/present.cpp`
(swapchain width scale), `src/features/stereo/gpu.cpp` (composition). Addresses are RVAs.
"static" = read in Ghidra, "live" = observed in the lab (instance `awr-wide`, gamescope
3840x1080@60), "measured" = from captures.

## Use

```ini
[stereo]
mode = sbs
output = full        ; read at start-up
[game]
display_mode = borderless   ; or windowed
```

With the glasses in 3D mode the desktop is 3840x1080: borderless gives 1920x1080 per eye in a
3840x1080 window. Windowed at 1920x1080 gives the same on a desktop at least 3840 wide. With
`output = full` and no stereo pair (mode off/left/right/alternate, menus, pause, Bink videos) the
engine's single image is copied into both halves, i.e. 2D in the glasses (live: both halves
identical in captures, `gpu duplicated=` counter).

## The game's window (static)

`app::WindowX86` (vtable `0x77be40`; the object pointer is in the global `0xd0ad38`, set on
WM_CREATE):

| offset | field |
|---|---|
| +0x10 | input object (mouse: +0x10 int, 0 = absolute cursor, else captured/relative) |
| +0x18 | HWND |
| +0x20 / +0x24 | client size the engine renders at |
| +0x30 | mode: 0 fullscreen (popup), 1 windowed, 2 borderless (popup at the monitor origin), 3 maximised |
| +0x138 | resize request (WM_SIZE sets 3 when the size changed) |

- `0x3b4b60` create(this, title, w, h, mode, monitor, flags): `AppWindowClass`, `CreateWindowExA`,
  centres a windowed window, stores w/h at +0x20/+0x24 at the end. vtable slot 9.
- `0x3b50b0` resize(this, w, h, mode, monitor, flags): restyles and `SetWindowPos`, same store.
  vtable slot 27.
- `0x3b39e0` window procedure: WM_SIZE stores LOWORD/HIWORD(lParam) at +0x20/+0x24 and raises
  +0x138; mouse messages 0x200..0x209 go to the input object with client pixels.
- Mouse handlers (`0x3b6ef0` move, `0x3b6bb0` buttons) normalise client pixels by +0x20/+0x24
  (getter `0x3b46d0`); in relative mode the move handler measures the distance from a saved
  screen point and warps back with `SetCursorPos`. `0x3b6b10` returns the cursor normalised by
  `GetClientRect` (real client). `0x3b7160` warps the cursor to a normalised position (+0x20 based).
- Resolution: `Game::Game` (`0x1ce4c0`) logs "Wanted resolution", reads the desktop size with
  `MonitorModes::get` (`0x3b4590`, call at `0x1ce7aa`) and for borderless (Game+0xdc == 2) uses
  it as the resolution (Game+0xd4/+0xd8), otherwise clamps to it. `Game::setDisplay` (`0x1d0e40`)
  does the same for borderless (call at `0x1d0e8b`) and hands w/h to the settings object, whose
  apply (`0x1e0380`) resizes the swapchain and calls the window's resize.

## What the mod does (output = full)

1. Swapchain created 2x wide, `GetDesc`/`GetDesc1` report the game's width (core/present, spike).
2. After create/resize the real window is set to a 2W x H client; the window object keeps W.
   The engine's width is what the game asked for, or half of it when 2W would not fit the screen.
3. WM_SIZE reaches the game as (real/2, h). If something sets the real window to the engine's own
   width it is re-widened on the next Present instead of halving again (the first build halved
   unconditionally and spiralled 3840 → 1920 → 960 → … when a resize to 1920 arrived 9.5 s in;
   live, run r1).
4. Absolute mouse positions reach the game as `x mod W`: over either half the game sees the
   position within that eye, so the OS cursor lines up with the menu in the eye it is over.
   Relative mode (mouse-look) is untouched. `0x3b6b10` gets the same mapping.
5. Borderless: while `Game::Game` (only if Game+0xdc == 2) or `Game::setDisplay` read the desktop
   size, `MonitorModes::get` reports half its width, so the engine's resolution is one eye.
6. All start-up hooks are now enabled in one MinHook batch (`hook_batch_begin/end`): enabling each
   separately suspends every thread, ~12 ms per hook under Proton, and the 13 hooks took 0.22 s,
   only 0.27 s ahead of the boot desktop query. Batched: all enabled 12 ms after load (live, r4).

## Verified (live, `awr-wide`, gamescope 3840x1080, level 15)

- Windowed 1920x1080: `create 1920x1080 mode 1 -> engine 1920x1080`, client 3840x1080, swapchain
  3840x1080 (game sees 1920), stable for the session (`stereo window` verb: engine 1920x1080,
  last WM_SIZE 3840 → 1920).
- Borderless at boot: `borderless at 3840x1080 desktop -> engine resolution 1920x1080`, window
  3840x1080, swapchain created 1920 → 3840. Live switch windowed → borderless (`cfg
  game.display_mode borderless`) before the fix: the game resized its swapchain to 3840 (one eye
  3840 wide); after: 1920.
- Capture of the gamescope output: both eyes side by side over the full 3840 width, near objects
  with visibly larger disparity than far ones; HUD in both.
- Pause menu: hovering "Options" at (2565, 799) — right half — selects Options in both eyes;
  hovering "Statistics" at (645, 841) — left half — selects Statistics (xdotool absolute moves).
- Mouse-look in gameplay (relative mode) still turns the camera.
- `mode = off` with `output = full`: identical halves.

## Not done

- Exclusive fullscreen (mode 0) with a 2x swapchain is untested: DXGI would need a 3840-wide
  display mode. Use borderless.
- Half SBS (`output = half`) still crops the centre of each eye; a proper 2:1 squeeze needs a
  shader pass.
- The OS cursor is drawn by the desktop over one eye only (it is not part of the rendered image).

![3840x1080 window, both eyes](img/stereo-window-3840.jpg)

*The gamescope output (3840x1080, downscaled) of run r2: windowed 1920x1080 per eye, the window
widened to 3840; left eye left, right eye right.*
