# First-person camera (prototype)

Goal: play Alan Wake Remastered in first person, including in stereo 3D (XReal glasses, the
stereo feature's `sbs` mode). This note is the investigation (camera system, player, aiming) and
the prototype `[camera] mode = first_person` (`src/features/firstperson.cpp`, extension point in
`src/core/camera.h`).

Status in one paragraph: **it works as a render-side first-person view.** During normal gameplay
the rendered camera sits at Alan's eyes (his animated head slot plus a small offset), looks where
the game's own third-person camera looks (converged on that camera's centre ray at 15 m), only
his head, hair and neck are left out of the rendered view (since 0.6.3, `hide = head`, section 6:
his arms and hands holding the flashlight and gun, his torso and legs stay, and his shadow keeps
the head; `hide = body/all` hide the body with the engine's render-object "hidden" flag, not with
the per-character hide mask, which stops "use" working: section 5), and the game is unchanged
underneath: walking, mouse look, aiming the
flashlight, focusing it on the Taken birds all play as before because every gameplay system still
reads the third-person camera. Cutscene, free, death and vehicle cameras and scripted shots inside
the player camera fall back to the game's camera (and the body comes back). It composes with
stereo: first person is a camera filter that runs before the stereo eye offset, so `sbs` renders
both eyes around the first-person eye point (measured depth-dependent disparity). What it is not
yet: a first-person game in every respect (aim parallax at close range, DOF focus and sound
listener still at the third-person camera, head bob only low-passed). Since 0.6.4 (`framing =
close`, the default, section 10) the arms are re-posed as a viewmodel: the flashlight in the
left hand at the lower left, the gun in the right hand at the lower right, the beam from the hand
to the screen centre, and a two-handed sight picture when aiming; `framing = classic` keeps the
third-person arms of 0.6.3 (they show only when aiming or looking down).

Addresses are RVAs (image base 0x140000000) of `Game_f_x64_EOS.exe` (Epic, 2026-11). "static" =
read in Ghidra (`re/scripts/Serve.java`), "live" = observed in the lab (instance `awr-fp`,
1920x1080, fixtures `awr-forest-night`, level 01/02 starts), "measured" = numbers from the lab.

![third person (top) and first person (bottom), level 15](img/fp-forest-third-vs-first.jpg)

## Quick use

```ini
[camera]
mode = first_person   ; third_person = the game's camera (inert)
fov = 0               ; horizontal degrees at 16:9 in first person, 0 = the game's (game.fov)
hide = head           ; head (default: arms, body, held items visible) | all | body | none
framing = close       ; close (default: viewmodel arms, section 10) | classic (0.6.3's arms)
hands_forward = 0.34  ; close: resting wrists this far ahead of the eye (m)
hands_drop = 0.65     ; ... this fraction of the half-height below the centre
hands_side = 0.40     ; ... this fraction of the half-width out to their side
hands_sight = 0.60    ; aiming: the gun hand this fraction of the half-height below the centre
hands_motion = 0.5    ; how much of the animation's own hand motion shows on top of the pose
hands_sway = 0.06     ; s: the hands trail the view's turns
anchor = head         ; head (animated) | body (root + eye_height)
smooth = 0.1          ; s, low-pass on the head's motion relative to the body
eye_up = 0.05         ; m above the head slot
eye_forward = 0.10    ; m ahead of it (camera's horizontal forward)
aim_distance = 15     ; m, look at this point of the game camera's centre ray (0 = parallel)
```

All live (`quiver lab call <i> cfg camera.mode first_person`). Lab verb: `quiver lab call <i> fp`
(status: active or why not, eye/head/root/game-camera positions), `fp slots N` (census of the
player's slots), `fp freecam on|off` (test aid: the debug free camera and back), `fp use` (the
game's current use target, i.e. what the interaction prompt is for, the game's hide mask on the
player and whether ours is applied), `fp usables [mask] [radius]` (game objects of a type mask near
the player: 4 = pickups, 0x40 = triggers incl. use triggers, 0x4d = everything "use" considers;
for finding pickups to test with), `fp hidetest on|off` (test aid: first person's hiding of the
player in third person too), `fp parts [bones]` (the player's render objects, their submeshes with
material effect and bind-pose height range, and with `bones` the skeleton's bone names),
`fp mask HEX` (test aid: the body's per-submesh draw mask), `fp give weapons|flashlight` (test aid:
what the game's Developer Menu cheats do: revolver, flare gun, shotgun; flashlight and batteries),
`fp vm` (framing = close: the wrists and their targets in camera space, aim factors, counters),
`fp vmtest on|off` (test aid: pose the arms in third person too, for an eye looking along Alan's
facing), `fp skel` (the animation skeleton: joints, parents, positions, slot table, render-bone
map), `fp slotm S...` (world transforms of slots), `fp peek ADDR [N]` (RE aid: bytes at an RVA or
address).

---

## 1. The camera system (static unless marked)

### CameraManager

Globals next to the getter `0x1d2540` (`mov eax,[0xb05e28]; ret`), created in `0x1d2580`:

| global | object | class / size |
|---|---|---|
| `0xb05e28` | camera **mode** (int) | 0 player, 1/2/6 free, 3 cinematic, 4/5 animation preview, 7 death |
| `0xb05df0` | "Player camera" (getter `0x1d2570`) | `PlayerCamera`, 0xee8 (ctor `0x29ade0`) |
| `0xb05e10` | "Cameraman camera" | plain `BaseCamera` 0xd00 (ctor `0x15d810`, vtable `0x752ad0`) |
| `0xb05e00` | "Free camera" | `FreeCamera` 0xd18 |
| `0xb05e08` | "Cinematic camera" | `CinematicCamera` 0xd00 |
| `0xb05e18` | "Death camera" | `DeathCamera` 0xd30 |
| `0xb05df8` | "Animation preview camera" | 0xd20 |
| `0xb05e20` | sound listener | `snd::Listener` |

- `CameraManager::update` `0x1d2c90` (from `GameMode` `0x34d200`): velocity for the listener, then
  `0x1d2fe0` updates the cameras of the current mode, then the sound listener is placed at **the
  mode's camera** (`0x5462b0`/`0x546300`: in mode 0 the PlayerCamera, i.e. third person).
- `0x1d2fe0`, mode 0: `PlayerCamera::update` (`0x29d380`), then the Cameraman camera gets the
  CameraMan effects transform (shake and recoil, global `0xaf8a00`) via `BaseCamera::setTransform`
  `0x15e210`, plus the PlayerCamera's FOV (`0x463c70`) and zoom (`0x463cb0`). Mode 3: the
  Cinematic camera (updated by `0x191940` whenever `0x191530` returns a running cinematic). 1/6:
  PlayerCamera still updated, plus the free camera. 7: death camera.
- `CameraManager::active(bool withEffects)` `0x1d2400` (about 100 callers: gameplay, AI, audio,
  renderer): mode 0 returns the PlayerCamera (false) or refreshes and returns the **Cameraman
  camera** (true); other modes return their camera. **The Cameraman camera is the view rendered in
  gameplay** (live: renderFrame's view is it in mode 0, a different View in mode 1 -> log lines
  "rendering the Cameraman camera" / "rendering another camera").
- `BaseCamera::setTransform(float[12])` `0x15e210` stores the float transform at **+0xcc8**
  (rows right, up, forward, position; getters `0x15d9f0` = +0xcc8, `0x15da00` = position +0xcec)
  and calls `0x15e650`, which widens it to doubles and calls `View::setViewToWorld` `0x15e2f0`
  (docs/stereo.md section 1). Gameplay reads the floats at +0xcc8; only the renderer reads the
  View's doubles (+0x248). That split is what the prototype relies on.

### PlayerCamera (the third-person camera)

Layout (ctor `0x29ade0`, update `0x29d380`):

| offset | content |
|---|---|
| +0xd08 | `CameraMan` (0x70, ctor `0x1728a0`): three `CameraShake`s and a `CameraRecoil`; its transform (first 12 floats) is the global `0xaf8a00` (getter `0x172bf0`); applied by `0x172db0` |
| +0xd10 | `TailCamera` (0x1b0, ctor `0x2ed2b0`): **the player-follow camera**, the base "camera mode" |
| +0xd20 | 8 x `AttachedCamera` (camera attached to an entity / bone) |
| +0xd60 | 8 x `RailCamera` |
| +0xda0 | 8 x `PathCamera` |
| +0xde0 | `RevealCamera` |
| +0xde8 / +0xdf0, +0xe08 / +0xe10 | two stacks (array, count) of active camera modes, blended in order |
| +0xe28 / +0xe2c | entity handle / bone index for a blend towards an attached pose (`0x179e30`) |

`PlayerCamera::update`: each camera mode's vtable +0x18 is `update(float xform[12]*, float*
fov, float* zoom)`; the TailCamera and the RevealCamera run first, then the two stacks (a mode
that reports "done" (vtable +0x20) is popped), an optional blend to the attached pose, the FOV
blended to the aspect (`0x463be0`), zoom clamped to 0.01..1, then the CameraMan adds shake and
recoil and `setTransform` is called. "Camera:*" tweakables (FOV 88, Zoom Factor 0.9, Near 0.2,
High/Mid/Low/Safe Pos, Switch Side Speed, Modern Central Offset Scale, Aim Speed Multiplier...),
"Camera:Collision *" and "Vehicle Camera:*" are its parameters (live: `tweak list Camera`).
`Aim:Max/Min Vertical Angle` = +/-60 deg is the pitch clamp.

- **Aim / zoom**: aiming (right mouse) pulls the TailCamera in over the shoulder and lowers zoom;
  both reach the Cameraman camera each update (static), so the game's aim zoom applies in first
  person too.
- **Cinematic moments** (slow-motion dodges and the bird attacks, sepia grade): stay in mode 0
  (live: first person stays active through them, `fp-focus-birds.jpg`).
- **Scripted shots while the player has control** (RailCamera / PathCamera / RevealCamera /
  AttachedCamera in the stacks): mode 0, but the game camera leaves the player. Live: the
  helicopter fly-by in level 15 put the game camera 13 m above Alan
  (`game_camera` y 59.8 vs head 46.9) for 2.4 s.
- **Cutscenes**: Bink videos (ModeSwitch's VideoMode, docs/levels.md) and in-engine cinematics
  (mode 3, Cinematic camera).
- **Vehicles**: the player's `Character +0x218` is a vehicle link (`0x159d60` = has a vehicle,
  `0x159dc0` = driving; used by the trigger code `0x153260` for OnPlayerEnterInVehicle). The
  vehicle camera itself ("Vehicle Camera:*" tweakables, CONTROLLER_DRIVE_*_CAMERA) was not traced:
  first person falls back while the link is set (static only; no driving section was reached).
- No first-person mechanism exists in the engine (no strings, no camera class for it). The debug
  free camera does (TOGGLE_FREECAMERA, `0x1d2b40`, "Free Camera:*" tweakables).
- `Renderer::renderView` `0x2ae2f0` places the focus/DOF from `CameraManager::active(false)`, the
  PlayerCamera's position (static): in first person the DOF focus is computed from the
  third-person camera.

## 2. The player

- Player manager `0x1873a0` -> +0x50 the `Character*` (docs/levels.md); +0x1c9 player flag.
  `Character` world transform at +0xbc (`0x2652c0`): its position is **1.08 m above the feet**
  (live: slot 30 sits at (0, -1.079, 0) below it).
- **Bone slots**: `Character::slotTransform(this, float out[12], int slot)` `0x179f10` (virtual):
  if the character is animated (vtable +0x2c8) it reads the pose (`+0x1b8 -> +8`): the slot table
  at pose+0x50 (int joint per slot, -1 = none; `0x184160`, `0x414a30`) and the slot's model-space
  transform at pose+0x160 + slot*0x30 (`0x414d10`), subtracts the controller's height (+0x1c0 ->
  +0xd0) and multiplies by the character transform. Otherwise it returns the character transform.
  `0x179e30` is the same by joint index. **Slot 0x1d = the head**: the "head heading" getter
  `0x17a420` and the Taken head-shot test `0x340580` both use it (static); live census
  (`fp slots 48`, Alan standing): slot 29 is the highest, (0.005, +0.569, 0.005) from the root,
  i.e. ~1.65 m above the feet; 27/28 just below it (+0.516/+0.465, neck), 32/33 (+0.62)
  shoulder-height attachments, 30 ground (-1.079), 19-26 legs, 4-18 arms and hands.
- **Head motion** (measured, head - root while walking on the ferry, 25 samples in 2.7 s): at rest
  +0.58 m; the first steps dip the head to +0.33 m and push it 0.3 m ahead of the root; steady
  walking bobs about +/-4 cm. Raw it is a lot of motion for a camera, hence `smooth`.
- **Hiding the player**: `Character +0x210` is a `CharacterHide` (+0 Character*, +8 mask, +0xc the
  mask last applied); `CharacterHide::set(this, bool on, u32 bits)` `0x1ef4f0` sets/clears bits and,
  when the mask changed, hides or shows: the character's render objects (render-object flag 8,
  `0x463180`), the weapon (`Character +0x220`, `0x340340`), the flashlight (`+0x230`, `0x22cb60`:
  mesh, and `Flashlight[0]` = hidden, which turns the beam off in `0x22f370`), and attachments
  (`0x308110`, `0x2d8760`, `0x2f1420`). Any low-16 bit hides; a mask other than 0, 2 or 8 also
  disables the character controller (`0x178dc0`, flags at +0x2c0), i.e. removes Alan from
  physics. Bits seen at the call sites: 0x10000 (about 20 script/cutscene sites), 0x40000, 0x10,
  4, 1, and **8 = the debug panel's "Hide character"** (`DebugPanel::AnimationPage` global
  `0xaf5d78` +0x146), which every Character update re-applies (`0x17e716`). Bit 8 hides meshes
  and keeps physics, but **it freezes the player's input reader** (`0x1ef6e0` "low 16 bits are 2
  or 8", read by GameMode `0x34dcb0`), which broke "use" in first person up to 0.6.1: section 5.
  The prototype now sets the render flags itself.
- Near plane: `Camera:Near` 0.2 m. With `hide = none` and the eye 0.1 m ahead of the head joint the
  head is mostly behind the camera, but the hair comes into view when the head moves ahead of the
  smoothed eye (walking, live: `fp-head-walk.jpg`). Section 6 covers what `hide = head` does.

## 3. Aiming and gameplay in first person

The prototype changes only what is rendered; the game keeps simulating its own third-person
camera. Consequences (live unless marked):

| what | behaviour |
|---|---|
| movement | relative to the (unchanged) game camera's heading, which is the view's heading: W walks into the view (live: 2 s of W moved the player 5.7 m along the view) |
| mouse look | unchanged (it turns the game camera, the view follows); pitch clamped to +/-60 deg |
| flashlight aim / focus | the game aims along its own camera's centre ray; the beam leaves the (hidden) left hand at the lower left and reaches the aimed point; "[F] Focus" works and burns the Taken birds (`fp-aim.jpg`, `fp-focus-birds.jpg`) |
| aim parallax | the game camera is ~2.9 m from the eye (measured: game camera (1383.26, 46.77, 2263.48), head (1385.54, 46.87, 2261.72)), behind and right of Alan. With `aim_distance = 15` the view looks at the 15 m point of that ray: the screen centre is on target at 15 m, off by roughly atan(0.5 m / d) for other distances d |
| gun | not tested (the fixture's Alan has no gun yet); same aiming path as the flashlight (static assumption) |
| dodge / cinematic moments | stay first person (mode 0) |
| interaction prompts, HUD, objective text | screen-space, unchanged; the red world-space threat arc still draws |
| use, pickups (batteries, ammo, supply cases) | the game picks the target from the player's position and facing and its own aim point, as in third person; works with every `hide` setting since 0.6.2 (section 5) |
| scripted shots in mode 0 | fall back to the game camera when it is more than 6 m from the head (live: helicopter fly-by, log "scripted shot" for 2.4 s, then "active") |
| cover | Alan Wake has none |
| sound | the listener stays at the third-person camera (static) |
| DOF / focus | computed from the third-person camera position (static) |

## 4. The prototype

### Where the transform is changed

The renderer's view of the gameplay camera is `View::setViewToWorld` on the Cameraman camera,
which the stereo feature already hooks (MinHook: one hook per function). Instead of a second hook,
`src/core/camera.h` is a small extension point that stereo's hooks call on the game thread,
whatever the stereo mode (off included):

- `camera_filter_add(order, fn(view, double m[12]))`: run for every `setViewToWorld` call, in
  ascending order, **before** the stereo eye offset. Stereo then treats the filtered matrix as "the
  game's" transform: its eye offset (and the per-eye re-application in `render_eye`) is applied on
  top of the first-person pose.
- `camera_prerender_add(fn(view))`: run at the top of each `renderFrame` with the main view, before
  stereo touches it (both eyes of an sbs update then share the projection set here).
- If stereo's signatures do not match, its hooks are not installed and first person reports
  `"hooks": false` (it needs them).

### What first person does (`src/features/firstperson.cpp`)

- Filter: only for the Cameraman camera (the global `0xb05e10`, resolved from `0x1d2430`), only
  while active: camera mode 0, a player character, no vehicle link, the head slot present (else the
  getter returns the root), and the game camera within 6 m of the head. Then the position becomes
  `root + lowpass(head - root) + up*eye_up + horizontal_forward*eye_forward`, computed once per
  presented frame so every set within a frame agrees; with `aim_distance > 0` the orientation is
  rebuilt to look at the game camera's ray point at that distance (world up kept, no roll).
- Pre-render: with `fov > 0`, the Cameraman camera's horizontal FOV (+0xcb0) is set from the 16:9
  value keeping its vertical FOV for the view's aspect (so hsbs's doubled aspect still works), and
  the projection is marked dirty. Rendering another camera clears "active" (the Cameraman camera
  stops being set in other modes, so the filter alone would not notice).
- Hide `head` (since 0.6.3, the default; section 6): the head's submeshes are left out of the
  main-view collection of the body's render object; nothing else is hidden. If that is not
  available it falls back to `body` below.
- Hide `body` / `all` (since 0.6.2, section 5): the visual half of `CharacterHide::set` applied directly, without
  touching the game's mask: render-object flag 8 on the body, its attachment slots, the inventory's
  linked entity and the held throwable, plus (`hide = all`) on the weapon and flashlight meshes
  only (the flashlight's own hidden flag, which also switches the beam off, is left alone). Not
  while the game's mask hides the character itself (low 16 bits set: scripts, cutscenes). A
  pass-through hook on `CharacterHide::set` puts ours back right after the game re-applies its
  own visibility (its applied-mask cache +0xc changed: e.g. a use animation hides the gun with
  0x10000 and shows everything after), and every main-view set checks the body, gun and
  flashlight flags (a gun drawn later). When first person stops (or `hide` changes),
  `CharacterHide::set` is made to re-apply the game's own mask (its cache invalidated).
- Inert with `mode = third_person`: the filter returns at once, the hide hook passes through
  (nothing of ours applied), the pre-render returns. Every address is behind a byte signature (`kSigs`); a mismatch disables the
  feature with a log line.

### Fallbacks, as established

| situation | how it is detected | established |
|---|---|---|
| loading, no player | player manager +0x50 empty / head slot table missing | live (log at every load) |
| Bink cutscene | VideoMode draws the video; whatever 3D frame is rendered behind it uses the rules here | not tested (the lab runs used `skip_videos = 1`) |
| in-engine cinematic | camera mode 3: a different View is rendered | static; same path as the free camera |
| free camera | camera mode 1 | live (`fp freecam on`: body and flashlight back, `fp-fallback-freecam.jpg`) |
| death camera | camera mode 7 | static |
| scripted shot in mode 0 | game camera > 6 m from the head | live (helicopter fly-by) |
| driving | `Character +0x218` vehicle link set | static |
| menus / pause | nothing special: the world behind the pause menu is whatever the camera rules give | not checked |

## 5. Use and pickups (fixed in 0.6.2)

Bug (user report, 0.6.1): in first person "use" (E, pad B) did nothing: no battery or flashlight
pickup, no supply case opened, although the game's prompt ("[E] Pick up batteries", "B Open") was
shown.

### Cause: the hide mask freezes the player's input reader

- **Reproduced live** (level 03 start area, supply case at (106.8, 118.1, -1721.9) and loose
  pickups found with `fp usables`; `fp use` shows the game's target): the prompt and the selected
  target are the same in third and first person, so targeting is not the problem. Pressing use:
  third person picks up; first person with `hide = all` or `body` does nothing; first person with
  `hide = none` opens the case; **third person with only our hide bit forced (old `fp hidetest`)
  does nothing**. So the hide bit, not the camera, blocks use. (A shotgun-ammo pickup pressed in
  first person was found picked up right after switching back to third person: probably the
  latched press, handled once the reader ran again; not reproduced.)
- **Static** (Ghidra): use is handled by `PlayerInputReader::update` `0x2a0940` (vtable
  `0x1407694b8` +0x18; RTTI "PlayerInputReader", Character +0x1a0): it calls the target selection
  `0x29fa40` -> `0x3220e0` (candidates of type mask 0x4d from `0x1d9190`: 4 pickups, 8, 0x40
  triggers, 1 characters; scored by distance <= 2.5-3 m, the player's facing (45 deg) and the aim
  point (60 deg), plus a ray check `0x321a10` from the camera position) and, when use is pressed,
  starts the use action (Character +0x298: `0x3207a0` pickup, `0x320820` trigger). GameMode's
  per-character pass `0x34dcb0` (from `0x34d200`) runs that reader only when
  `0x1ef6e0(hide mask)` is false, and `0x1ef6e0` is true exactly when the mask's low 16 bits are
  2 or 8: the debug "Hide character" bit freezes the reader, and when it is frozen `0x34dcb0`
  calls `0x29fa40` itself, which is why the prompt kept showing. `0x1ef6c0` ("hidden", used by
  other code) excludes mask 8, and no reader of the mask tests bit 8 for use (the use path only
  tests 0x10000).
- **Bisected live** (a test verb applying each effect of `CharacterHide::set` separately in third
  person): render-object flag 8 on the body (hides Alan), the attachment slots, the inventory's
  linked entity, the held throwable, the weapon and the flashlight hide: with each, use still
  worked. Only the mask value blocks it. No other mask bit both hides the body and keeps the
  player working: any low-16 value other than 0, 2 or 8 disables the character controller
  (`0x1ef4f0` -> `0x178dc0`), and bits >= 0x10000 do not hide the body.

### Fix

Hiding no longer uses the mask: section 4 "Hide". RVAs used (all behind `kSigs`):
`0x463180` RenderObject flags (8 = hidden), `0x2d8760` attachment slots (inventory +0x48),
`0x308110` inventory +0x18 linked entity, `0x2f1420` held throwable (Character +0x240), the
weapon's mesh at holder +0x30/+0x38 and the flashlight's at +0xc0/+0xc8 (from `0x340340`,
`0x22cb60`), the apply sequence of `0x1ef4f0` at `0x1ef602` (pins the offsets), and the getters
`0x17a5c0`, `0x17acb0`, `0x203220`, `0x21cad0`.

### Tested (live, 0.6.2 build, lab `awr-fp`, 1920x1080)

| case | result |
|---|---|
| first person, `hide = all`, level 03 supply case | "Open" -> case opens; Alan, gun and flashlight meshes stay hidden after the animation (`hide_applied` counted the re-apply) |
| first person, `hide = body`, level 03 | batteries picked up |
| first person, `hide = none`, level 03 | revolver ammo picked up, case empty |
| first person, `hide = all`, level 15 (flashlight lit) | "Picked up batteries", HUD 2 -> 4; beam on, no flashlight mesh |
| first person, `hide = body`, level 15 | "Picked up shotgun ammo"; flashlight mesh visible at the lower right |
| third person (control), level 15 | "Picked up revolver ammo", Alan and flashlight visible |
| third person with our hiding (`fp hidetest on`), level 03 | "Picked up shotgun ammo", Alan stays hidden |
| switching first -> third person, and `hide` all -> body -> none live | body, gun and flashlight back as the game has them (`hide_restored`) |
| walking in first person after the change | works (S for 1.2 s moved 2.2 m) |

Not tested: a gamepad (the lab sends keys; the prompt shows E or pad B for the same action, and
the frozen reader handled both), a flashlight pickup proper (the tested pickups were batteries,
ammo and a supply case; every type-4 pickup starts through the same `0x3207a0` call, static), use while a cutscene's mask
hides the character (ours is not applied then, by design).

## 6. Arms and body in view: `hide = head` (0.6.3)

Goal: see Alan's arms and hands holding the flashlight and gun as the third-person animations
pose them, and his torso and legs when looking down, without the head, hair or neck in view.
Result: `[camera] hide = head` (the new default) leaves exactly the head's submeshes out of the
main view; everything else of Alan, the held items and the beam render as in third person, and
his shadow still has a head. Lab instance `awr-fp`, 1920x1080, build 0.6.3.

### How Alan is built (static + live)

- **One skinned mesh, one render object.** `Character::init` `0x1794a0` creates the body's render
  slot at Character +0x2d8 with `0x2afcc0`: +0 present, +8 a `rend::Object` (0x1f8 bytes, ctor
  `0x46cd00`, vtable `0x785d48`), +0x10 a `rend::BoneArray` (`0x4b21d0`: +8 bone matrices, +0x10
  skinning matrices, +0x18 count). `Character vtable +0x2a0` returns that slot (what the hiding
  code already used). `rend::Object`: +0x24 flags (8 = hidden, `0x463180`), +0x80 the `rend::Mesh`
  (0x280 bytes, ctor `0x4ae770`), +0x88 world transform, +0xe8 the BoneArray, +0xf8 per-submesh
  skinned vertex buffers (double-buffered by frame parity, `0x46d650`), +0x158 **per-submesh draw
  mask**, +0x178 LOD weights.
- **The mesh has submeshes, one per material.** `rend::Mesh` +0x28: up to 32 submesh pointers,
  +0x128 their count, +0x238/+0x240 bones (0x80 each: name char[0x40], inverse bind at +0x40,
  bounding sphere at +0x70), +0x244 LOD count, +0x268..+0x27c bounding box. A submesh: +8 its
  material (effect name: `StandardMaterial.rfx`, `Hair.rfx`, `Skin_d3t.rfx`, `Character.rfx`),
  +0x10 vertex format (4-byte entries {stream, type, semantic, index}, count at +0x50,
  `0x4ba240`), +0x54 stride, +0x80 vertex buffer (vtable +0x30 maps the CPU copy, +0x38 unmaps:
  the debug normal drawer `0x4d0c30` does this), +0x90 LOD, +0x94 vertex count, +0x9c offset.
- **The draw mask is checked per view pass.** The main-view collection `0x4cad50` (reached through
  `rend::Object` vtable +0x40 = `0x46dfb0`) skips submesh i when bit i of +0x158 is clear
  (`test [r13+0x158], r14d` at `0x4cbf42`), and so does the shadow collection `0x4cd150`
  (`0x4cd6a5`). The ctor sets the mask to all ones. Skinning happens inside these collections
  (per submesh, the first pass that draws it skins it: `0x46d6f0`, `0x4c5ec0`).
- **Live census** (`fp parts`, level 03 body, 14 submeshes, 152 bones, one LOD; isolated with
  `fp mask` in third person, `fp-head-parts.jpg`; heights are bind-pose vertex ranges in metres
  above the feet, the mesh is 1.87 m tall):

| # | effect | height (m) | what (live, isolated) |
|---|---|---|---|
| 0, 4 | Hair.rfx | 1.60-1.73, 1.61-1.87 | hair (scalp layer, hair cards) |
| 1, 2, 3, 7 | Character.rfx | 1.72-1.75 | eyes, eyelashes (small) |
| 6 | Skin_d3t.rfx | 1.46-1.84 | head skin: face, ears, **neck down to the collar** |
| 8, 9 | Skin_d3t.rfx | 1.64-1.69, 1.65-1.67 | teeth, tongue |
| 5 | Skin_d3t.rfx | 0.88-1.08 | both hands (T-pose, x +/-0.64) |
| 10 | Character.rfx | 0.00-1.00 | jeans and shoes |
| 11 | Character.rfx | 0.93-0.95 | small part at the left wrist (watch) |
| 12 | Character.rfx | 0.83-1.59 | jacket (incl. sleeves) |
| 13 | Character.rfx | 0.94-1.65 | hoodie and its hood |

  The order differs per level (level 15's mesh has 15 submeshes with a `StandardMaterial` one
  first, live), so parts are not identified by index. Bones are named (`Head`, `Neck0`, `Neck1`,
  `FBone_*` face rig, `L_Hand`, `Jkt_*` jacket, `Hood`...): `Neck0`'s sphere centre is at 1.607 m,
  `Head` 1.723 m, the eyes (`FBone_L_Eye`) 1.732 m and 0.098 m forward (live).
- No attachment slots, linked entity or held throwable were present in these levels; the
  flashlight (3 submeshes) and the weapon are their own render objects (live).

### What `hide = head` does (`src/features/firstperson.cpp`)

1. **Find the head's submeshes** once per mesh: map each submesh's vertex buffer (as `0x4d0c30`),
   read the bind-pose position (the float3 of semantic 0) and take the lowest vertex. A submesh is
   "head" when all its vertices are above the neck threshold: `Neck0`'s height minus 0.2 m (1.41 m;
   without that bone, 75 % of the mesh height). Live result in levels 03 and 15: `0x3df` = hair x2,
   eyes x4, head skin, teeth, tongue (log line `firstperson: hide = head: head parts ...`). The
   hood (1.65 m) stays because the hoodie submesh reaches 0.94 m.
2. **Hide them in the main view only**: a hook on `0x4cad50` clears those bits of the player's
   body's +0x158 for the call and restores them after. The shadow collection never sees the
   change, so the head still casts its shadow (live, `fp-head-shadow.jpg`: third person with
   `fp hidetest on`, the body has no head, the shadow on the rock has one). Measured over ~15000
   collections: all on the game thread (`head_collects_other_thread` 0) and all for the Cameraman
   view (`head_collects_other_view` 0), so there is no race with another pass.
3. **Nothing else is hidden**: the flashlight and gun meshes, the beam, the hands. The game's hide
   mask is never touched (use and pickups as in section 5); when the game's mask hides the
   character (scripts, cutscenes) we do nothing.
4. **Fallback**: a signature mismatch (`kHeadSigs`: the collection's prologue, `0x46dfb0`, both
   mask tests, `0x4d0c30`'s layout, the format reader, the bone fields), a failed hook, a mesh with
   1 or more than 32 submeshes, an unreadable vertex buffer, or no head / everything classified as
   head: `hide = head` hides the body as `hide = body` does, with a log line.
5. **Inert** in third person: the hook compares one pointer and passes through (live:
   `head_collects` unchanged over 60 frames in third person, mask `0xffffffff`, flags unchanged).

Bone collapse (scaling the head bones to zero in the skinning palette, BoneArray +0x10 from
`0x4b2390`) was not needed: the skinned vertices are shared by the main and shadow passes
(skinned once per frame, `0x46d6f0`), so collapsing bones would have removed the head from the
shadow too, while the submesh mask is per pass.

### Camera placement against the body

- The eye (head slot 0x1d + 0.05 m up + 0.10 m along the view's horizontal forward) lands on
  Alan's eyes: in bind pose the head joint is at 1.668 m, so the eye is at ~1.72 m and 0.10 m
  forward vs the eyes' 1.73 m and 0.098 m (static from the bone data, live census).
- The nearest remaining geometry is the collar (top of the hoodie and jacket, 1.59-1.65 m, ~0.1 m
  behind and 0.1-0.25 m below the eye) and the hood behind it. Looking forward or down to the pitch
  limit it stays out of view; once (stereo capture at the level 03 spawn while starting to aim with
  the camera pitched down) the open collar showed at the right edge as a dark ring with jacket
  fabric (`fp-head-stereo-hsbs.jpg`), not reproduced in a later turn-into-aim sequence
  (`fp-head-turn-aim.jpg`). The open neck is the artefact of hiding a submesh that runs down into
  the collar.
- `eye_forward` 0.15-0.25 instead of 0.10 (live, same frame, aiming down): the hands and the
  flashlight leave the frame at 0.20 and beyond; 0.10 stays the default.
- Near plane `Camera:Near` 0.2 m (PerspectiveView +0xcbc) left as is: no geometry was cut in any
  capture (the hands when aiming are ~0.4 m from the eye); `kViewNear` stays unused.
- Walking: the head moves ahead of the smoothed eye (`smooth = 0.1`); with `hide = none` the hair
  then hangs into the top of the view, with `hide = head` the view is clean (live, same frame,
  `fp-head-walk.jpg`).
- The torso and legs are **not** in view even at the pitch limit (-60 deg, the game camera's
  clamp): they are straight below the eye, and the bottom of the frame is ~84 deg down; the hand
  and flashlight show at the bottom (live, `fp-head-down.jpg`).

### Arms in view

| situation | what shows (live) |
|---|---|
| looking ahead, flashlight lowered (default pose) | nothing of Alan: the beam enters from below the frame |
| looking down ~40 deg | left sleeve, hand and flashlight at the lower left |
| looking down 60 deg (limit) | hand and flashlight at the bottom |
| aiming the flashlight (right mouse), `fov = 0` | both hands at the bottom edge |
| aiming the flashlight, `fov = 100` | hands, sleeves and flashlight in frame (`fp-head-aim-flashlight.jpg`) |
| aiming with the revolver | revolver in the right hand, flashlight in the left, both sleeves (`fp-head-aim-gun.jpg`; `hide = body` shows them floating) |
| turning into aim while looking down | hands, revolver and flashlight throughout (`fp-head-turn-aim.jpg`) |
| supply case "Open", "Pick up batteries" | works (10 -> 12 batteries); the pickup gesture is not in frame |
| stereo `sbs` / `hsbs` | both eyes show the hands; the head is hidden in both (the hook runs for every main-view collection) |

Proposal, not shipped as a default: `fov = 90-100` keeps the hands in frame when aiming without
moving the aim point (the view still converges on the game camera's ray at `aim_distance`). A
pitch bias or a lower eye would bring the walking pose's hands in but moves the screen centre off
the aim point or the eye off the eyes.

### Tested (live, 0.6.3 build, lab `awr-fp`, 1920x1080; frames compared on the virtual clock)

| case | result |
|---|---|
| level 03 and level 15 (`awr-forest-night`) start, looking ahead | identical to `none`/`all`/`body`: no body in frame |
| looking down 45 / 60 deg | sleeve, hand, flashlight; no head, hair or collar |
| walking (W, frames 20 and 50 after the press) | clean; `none` shows the hair at the top |
| aiming the flashlight, level 15 bird fight | hands in frame, beam and "[F] Focus" unchanged |
| revolver (`fp give weapons`) aiming | revolver and flashlight in the hands |
| use and pickups, level 03 supply case | case opened, batteries picked up |
| shadows (third person, `fp hidetest on`) | body headless, shadow with the head |
| third person | inert (no collection hits, mask and flags untouched) |
| stereo `sbs`, `hsbs` | composes; head hidden in both eyes |

![submeshes of Alan's body, each alone (level 03, third person)](img/fp-head-parts.jpg)
![third person: normal (left) and with the head hidden as in first person (right): the shadow on the rock keeps the head](img/fp-head-shadow.jpg)
![walking: hide = head (left), hide = none (right, the hair in view)](img/fp-head-walk.jpg)
![aiming the revolver: hide = head (left), hide = body (right)](img/fp-head-aim-gun.jpg)
![aiming the flashlight: fov 0 (left), fov 100 (right)](img/fp-head-aim-flashlight.jpg)
![looking down at the pitch limit: hide = head (left), none (right)](img/fp-head-down.jpg)
![stereo hsbs, aiming at the level 03 spawn: hands in both eyes; the open collar at the right](img/fp-head-stereo-hsbs.jpg)
![turning into aim while looking down (frames 6 apart)](img/fp-head-turn-aim.jpg)

### Risks and next steps

- The head-skin submesh includes the neck down to the collar: when the eye looks steeply down
  into the collar the opening can show (seen once). Options: a slightly lower near plane does not
  help; collapsing only the `Head`/`FBone_*` bones would keep a neck stump in view; a dark
  "collar cap" is not available in the mesh. Moving the eye 0.05 m further forward only while
  pitched below -45 deg would hide it at the cost of the hands at the bottom.
- The classification relies on bind-pose heights; a costume whose head piece reaches below the
  neck threshold (a scarf, a hat with a collar) would stay visible, and a costume whose torso part
  sits entirely above it would be hidden. Each new mesh is logged with its height ranges.
- Reflections and the cube probe: the main collection only saw the Cameraman view for the player
  (`head_collects_other_view` 0); a mirror that does render him would use the same path and show
  him headless.
- Torso and legs: to see them the view must pitch beyond -60 deg (a first-person pitch range on
  top of the game camera's, applied only to the rendered view) or the eye must move back above
  the shoulders; neither was prototyped.
- Hands while walking: a camera-relative arm pose (override the arm bones' pose so the flashlight
  hand sits in the lower left of the view) would be the viewmodel route; it needs the animation
  pose write path (`Character +0x1b8 -> +8`, pose +0x160 per slot) and would change the
  third-person pose too unless restricted to the render.

## 7. Evidence

- Third vs first person, night (`fp-forest-third-vs-first.jpg`, top/bottom, level 15 start; two
  runs at the same spot) and day (`fp-day-third-vs-first.jpg`, level 02 ferry, toggled live).
- Aiming the flashlight (`fp-aim.jpg`, "[F] Focus" prompt, beam from the lower left towards the
  target) and focusing it on the bird swarm in a slow-motion cinematic moment
  (`fp-focus-birds.jpg`).
- Fallback: free camera (top) and back to first person (bottom) (`fp-fallback-freecam.jpg`).
- Stereo: `[stereo] mode = sbs, output = hsbs` with first person (`fp-stereo-hsbs.jpg`). Measured
  disparity between the halves (`re/scripts/stereo_disparity.py`, convergence 4 m): far field
  (trees, mountain) -8.75 px, the car (~5 m) -7.0 px, the ground just ahead -5.9 px, HUD 0:
  depth-dependent, i.e. both eyes are offset around the first-person eye point.
- `fov = 100` widens the first-person view live (checked by eye; not measured).
- Log (`torchbearer.log`): `firstperson: active / no player / head slot unavailable / not the
  player camera / scripted shot`, and `rendering the Cameraman camera / another camera`.

![first person in stereo (hsbs)](img/fp-stereo-hsbs.jpg)
![fallback: free camera, then first person again](img/fp-fallback-freecam.jpg)
![day: third person (top), first person (bottom)](img/fp-day-third-vs-first.jpg)

## 8. What works, what doesn't

Works (live): walking, mouse look, aiming and focusing the flashlight, use and pickups (since
0.6.2), Alan's arms, hands, held flashlight and revolver, torso and legs with only the head hidden
(since 0.6.3, section 6), the bird fights including cinematic moments, level loads, switching modes live, the free-camera fallback and back, scripted
helicopter shot fallback, stereo sbs (hsbs) around the first-person eye.

Doesn't, or not yet:

- **Aim parallax**: the game aims from its third-person camera; the screen centre matches the
  aim point only at `aim_distance`. Close targets (a Taken at 3 m) will be off by ~10 deg.
- **Arms**: fixed in 0.6.4 by `framing = close` (section 10). With `framing = classic` the hands
  are the third-person animation's (they show when aiming or looking down 40 deg or more). With
  `hide = all` only the beam shows; `hide = body` keeps the flashlight and gun floating.
- **Head motion**: the walk-start dip (25 cm) and sway come straight from the animation;
  `smooth = 0.1` softens it but does not remove it; `anchor = body` is steady but ignores
  crouching animations.
- **Pitch** stays clamped at +/-60 deg (the game camera's limit).
- **DOF focus and the sound listener** stay at the third-person camera (static).
- Probably no player shadow with `hide = body/all` (render-object flag 8; not checked). With
  `hide = head` the shadow is complete (live, section 6).
- Not tested: guns, driving, Bink and in-engine cutscenes in first person (static reasoning only),
  death, scripts that hide the character for their own reasons while first person is active (ours
  stands aside while the game's mask hides the character, by design).
- A scripted shot that stays within 6 m of the head (a close "look at" camera) is rendered first
  person, looking where that shot looks.

## 9. Next steps (prioritised)

1. **Aim from the eye**: find the game's aim ray (the flashlight/gun target: TailCamera aim
   state, `Aim:*` / adhesion code, "Light:Aim beam" users) and either feed it the first-person
   camera (the float transform at +0xcc8 of the Cameraman camera, read by `0x1d2400(true)`
   callers) or converge the view on the actual hit point each frame instead of a fixed distance.
2. **Head and body**: done in 0.6.3 (`hide = head`, section 6). Left: the torso and legs are below
   the frame even at the pitch limit; see section 6 "Next steps" (a first-person pitch range, a
   camera-relative arm pose).
3. **Viewmodel**: done in 0.6.4 (`framing = close`, section 10). Left: section 10 "Rough edges".
4. **Cutscenes**: verify mode 3 and Bink fallback in a real in-engine cinematic; blend between the
   game's shot and the eye instead of cutting (a 0.3-0.5 s blend on mode changes); decide per
   scripted-shot type (Reveal vs Attached) instead of the 6 m rule.
5. **Motion comfort** (glasses): stronger vertical smoothing during walk starts, optional
   "horizon lock" (no roll; already none), an option to drop head bob entirely (body anchor with
   a crouch-aware height), keep DOF off or focus it from the eye, and the sound listener at the eye.
6. **Vehicles**: decide whether driving should be first person (cockpit view needs the car's
   interior) or stay third person (current).
7. Gun recoil and camera shake: they are in the game camera's rotation, so they reach first person
   as rotation only; check they feel right.

## 10. Close framing: the arms as a viewmodel (0.6.4)

Goal (user request): Resident Evil 7 / Village / Requiem style first person: hands, flashlight and
gun always in frame and close to the lens (flashlight hand lower left, gun hand lower right, both
together when aiming), a moderate field of view, the beam visibly from the hand in view, a sight
picture when aiming, subtle bob and sway. Result: `[camera] framing = close` (the new default);
`classic` is 0.6.3's framing. Lab instance `awr-fp`, 1920x1080, build 0.6.4, virtual clock 62.5 Hz.

![idle, level 15 (flashlight lit, revolver): classic (left), close (right)](img/fp-close-idle.jpg)
![aiming the revolver with the flashlight: classic (left), close (right)](img/fp-close-aim.jpg)

### Camera only? (measured, ruled out)

- The resting animation holds the flashlight hand ~0.55 m below the eye and the gun hand hanging
  at the hip, 0.1-0.4 m *behind* the eye plane (live, `fp vm`, wrists in camera space, x right,
  y up, z ahead, level 15: left (-0.21, -0.53, +0.27), right (+0.31, -0.53, -0.39) m). The game's
  view is 79 deg wide at 16:9 (tan of the vertical half-angle 0.465, live), so the bottom edge is
  25 deg below the centre; the wrists are 55-65 deg below it and the gun hand is behind the lens.
- Lowering the eye 0.3 m and widening to 95 deg (`eye_up = -0.3`, `eye_forward = -0.05`,
  `fov = 95`, live) brings a sleeve to the bottom edge, not the hands, and puts the eye in the
  chest. Pitching the view relative to the aim ray would move the screen centre off the aim
  point. So the arms have to move: a viewmodel pose.

### Where the pose comes from (static, Ghidra, and live)

- **Animation and its outputs.** `Character::updateAnimation` `0x17f600` animates in place or
  (global byte `0xaf9b30` = 1, live: the asynchronous path is the one used) queues a
  `CharacterAnimationGameJob` (vtable `0x754ec0`, run `0x17c220`). The result goes out in
  `Character::postAnimation` `0x17c080` (in the synchronous path: inline in `0x17f600`):
  1. `0x184080(anim, buf, 0x100)` (`lea r8,[rcx+0x48f8]; mov rcx,[rcx+8]; jmp 0x414a40`) writes the
     model-space pose of every render bone, `buf[i] = model[map[i]]` (map: int array at anim
     +0x48f8, count +0x4900; each 3x4 built from the joint's 0x30-byte qs-transform by
     `0x59521c`);
  2. `BoneArray::setBone` `0x168a50` copies them into the render BoneArray (Character +0x2e8, +8
     the bone matrices) and marks the render object dirty; the skinning palette is
     `inverse bind x bone` (`0x4b2390`, from `0x46d740` / `0x46dff0`);
  3. the flashlight update `0x22e590` and the weapon update `0x341890` read
     `Character::slotTransform` `0x179f10` (slot 0 = flashlight, 9 = weapon) and place their
     meshes, the flashlight's lights (+0x10 light, +0x18 / +0x20 spot lights, +0x28 beam volume)
     and **the light-damage cone** `0x22c3e0`, built from the same slot-0 transform (position
     +0x24 and axes; objects in the cone get light through `0x315070` after a visibility ray). The
     beam is slot 0's forward row (live: within 3 deg of the camera's forward when idle,
     `fp slotm 0`); the barrel is slot 9's forward row to ~15 deg (live, aiming).
- **Skeleton** (live, `fp skel`): the animation's skeleton instance is [Character +0x1b8] +8; its
  +0x50 is the slot table (int joint per slot), its [+8] the pose, whose [+0] is the skeleton:
  +8 int16 parent indices, +0x10 joint count (157 for Alan), +0x28 reference pose. Names come
  from the render bones (mesh +0x238 through the render map): `L_Shoulder` 5 -> `L_UpperArm` 6
  (`L_UpperArmTwist` 7) -> `L_ForeArm` 8 (`L_ForeArmTwist` 9) -> `L_Hand` 30 (fingers 31-46);
  `R_Shoulder` 10 -> `R_UpperArm` 11 (12) -> `R_ForeArm` 13 (14) -> `R_Hand` 47 (48-64);
  `SpineThorax` 4, `Neck0` 15, `Head` 17; jacket panels `Jkt_L/R_Shoulder` 127 / 142 (children of
  the chest) with Front / Back / Rib chains. Slots used (semantic, live): 0 flashlight (joint 31),
  1 left hand (30), 4 left forearm (8), 6 left upper arm (6), 9 weapon (48), 11 right hand (47),
  14 right forearm (13), 16 right upper arm (11), 0x1d head (17).
- `Character::jointTransform` `0x179e30` (a joint's world transform), the controller height
  (Character +0x1c0 -> +0xd0, `0x4083f0`, subtracted by both getters) and the game's frame time
  (`0x1df650`; all first-person smoothing uses it since 0.6.4, so it follows game time and the
  lab's virtual clock) complete what the pose needs.

### What `framing = close` does (`src/features/firstperson.cpp`, "framing")

The pose itself is not rewritten; its two outputs are, with the same rigid per-segment deltas
(upper arm, forearm, hand, clavicle, jacket panel, per side), computed once per animation update:

1. **Hook `0x184080`** (pose export): after the original, for the player's animation only, each
   render bone of an arm segment is multiplied by its delta (model space).
2. **Hook `0x179f10`** (slot transform): the player's slots whose joint lies in an arm segment
   (flashlight, weapon, hands) get the delta in world space, so the flashlight mesh, its lights and
   beam, the light-damage cone, the gun and its muzzle flash follow the posed hands.
3. **The deltas** (`vm_compute`, at the export): the hands' frame is the first-person camera of the
   last main-view set (kept relative to the character so walking does not leave the hands behind,
   low-passed by `hands_sway`, its pitch limited to [-70, +30] deg). Per arm that holds its item
   (flashlight / weapon mesh shown; an empty or game-hidden hand fades back to the animation in
   0.15 s, so use and pickup gestures play as animated):
   - **wrist target**: the animated wrist is split into a slow part (0.25 s low-pass) and a fast
     part. The slow part is replaced by a pose, "rest" (`hands_forward`, `hands_drop`,
     `hands_side`) or "aim" (the gun hand at `hands_sight` below the centre; the flashlight hand
     keeps its animated offset from the gun hand, so the two-handed grip stays), blended by how
     far the animation has raised that arm (wrist height above the shoulder / arm length: -0.62
     to -0.95 at rest, -0.07 to 0 aiming, live). The fast part (recoil, reload, the walk's swing)
     is added back, scaled by `hands_motion`, at most 5 cm;
   - **shrug**: the clavicle takes 35 % of the arm's swing;
   - **two-bone IK**: shoulder fixed; the elbow on the side of the animated elbow swung rigidly
     with the arm; the upper arm maps the arm's plane onto the new one and the forearm only bends
     (a shortest-arc turn per bone twisted the elbow, live);
   - **hand orientation**: the flashlight hand keeps its animated orientation (the animation
     already points the beam at the camera's target), the gun hand turns with the forearm; then
     each turns towards its aim, measured on the low-passed item direction so a recoil kick still
     shows: the flashlight to the view's aim point (the game camera's centre ray at
     `aim_distance`, where the screen centre converges), at most 75 deg; the gun to the aim point
     when aiming and to a "ready" direction ~11 deg lower when not, at most 80 deg;
   - **jacket**: the `Jkt_*_Shoulder` panels follow half the upper arm's turn (they are skinned up
     into the armpits).
4. **Near plane** 0.05 m instead of the game's 0.2 m (`Camera:Near`, PerspectiveView +0xcbc) while
   first person is active with `close`: the sleeves come within 0.2 m of the eye and the near
   plane cut them open (the inside of a cut sleeve showed as a hollow ribbon of cloth, live). The
   game's value goes back when first person stops (live: 0.05 in close, 0.2 in classic and third
   person).
5. **Field of view and eye**: the game's own FOV (79 deg at 16:9, live) is already moderate and
   stays (`fov` still overrides it); the eye stays at Alan's eyes; head bob is the head's own
   motion low-passed as before (`smooth = 0.1`).
6. **Fallback / inert**: `kVmSigs` (`0x184080`, `0x414a40`, `0x179e30`, `0x4083f0`, `0x184680`)
   and a skeleton check (each side upper arm -> forearm -> hand, attachment below the hand) must
   pass, else `close` keeps the animated arms (0.6.3's framing) with a log line. Third person and
   `classic`: the export hook compares one pointer and returns, the slot hook compares one pointer
   (live: no computes or applies over 120 frames in either, near plane 0.2).

### Tested (live, 0.6.4 build)

| case | result |
|---|---|
| idle, level 15 (flashlight lit, revolver from `fp give weapons`) | flashlight hand lower left, beam from it to the centre, revolver lower right (`fp-close-idle.jpg`) |
| walking (W, frames 16 apart) | hands stay in the lower frame, swinging a few cm with the walk; the view bobs with the head (`fp-close-walk.jpg`) |
| aiming flashlight + revolver | two-handed, flashlight under the revolver, rear sight below the centre, beam along the barrel; classic shows the revolver at the bottom edge (`fp-close-aim.jpg`) |
| firing (revolver, 3 shots, 42 -> 39) | muzzle flash at the posed muzzle; recoil kicks the gun hand up (`fp-close-fire.jpg`, top) |
| reload (R) | the gun hand comes in to the centre and back (`fp-close-fire.jpg`, bottom) |
| use / pickup, level 03 supply case | "Open" opens the case, "Pick up batteries" 10 -> 12; the game hides the gun during the gesture, so the right arm plays its animated reach into the case (`fp-close-pickup.jpg`) |
| looking down (pitch limit) | both hands, the beam's spot, the revolver; classic shows the flashlight's end (`fp-close-look.jpg`, top) |
| looking up | the hands drop out at the bottom (hands' frame capped at +30 deg); the beam from below (`fp-close-look.jpg`, bottom) |
| "[F] Focus" on the bird swarm, level 15 | the prompt shows with the beam on the birds, as in classic |
| stereo `sbs`, `hsbs` | both eyes show the same posed hands (one pose per update) (`fp-close-stereo-hsbs.jpg`) |
| `classic`, third person | no computes or applies, near plane 0.2 (counters, live) |

![walking: four frames 16 apart (close)](img/fp-close-walk.jpg)
![firing (muzzle flash, recoil) and reloading (close)](img/fp-close-fire.jpg)
![use: open the supply case, reach, "Picked up batteries" (close)](img/fp-close-pickup.jpg)
![looking down (top) and up (bottom): classic (left), close (right)](img/fp-close-look.jpg)
![stereo hsbs with close framing](img/fp-close-stereo-hsbs.jpg)

### Rough edges

- **Light damage follows the posed beam** (static: `0x22e590` passes the slot-0 transform it got
  from `0x179f10`, now posed, to the damage cone `0x22c3e0`; live: the slot hook applies every
  frame and the beam ends at the screen centre). Not measured: damage against a Taken. The
  flashlight aims at the convergence point (`aim_distance`), so section 3's parallax applies to
  the beam as to the reticle.
- **Shadow**: the arms' shadow is posed too (main and shadow passes share the skinned vertices):
  Alan's shadow holds his arms up in front.
- **Big swings**: in level 15's combat idle the gun hand hangs ~0.4 m behind the eye, so the arm
  swings ~150 deg; the sleeves stay whole (near plane, plane-keeping IK, shrug, jacket panels),
  but the revolver points steeply up-forward in the lower right (`fp-close-idle.jpg`) and the
  wrist bends visibly when a hand is turned far.
- **Recoil** shows strongly (the animation's kick plus the hand's turn); `hands_motion` scales
  only the translation.
- Dodges, cinematic moments and the bird attacks' slow motion go through the same pose; nothing
  special-cases them. The hands leave the frame when looking up past +30 deg.
- Not tested: the shotgun and flare gun (the left hand keeps its animated offset from the right,
  so it should hold the fore-end), throwables (thrown from the posed hand), a gamepad, damage
  numbers.
