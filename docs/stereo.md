# Stereoscopic 3D: findings, measurements and the road to full SBS

Target: XReal Air 2 Ultra in 3D mode, a **3840x1080 full side-by-side** signal (left half to the
left eye, right half to the right eye, each a 1920x1080 16:9 view), with **both eyes rendered from
the same simulated frame**. Prior art: Ishimura (Dead Space), which alternates eyes on consecutive
engine frames (`ds-decomp/docs/18-stereo.md`, `28-full-sbs.md`, `49-...`).

Status of this spike, in one paragraph: the camera lever is found and verified (eye offset is a
translation, disparity inversely proportional to depth, scales with separation, convergence puts
the screen plane where configured). **Same-update dual rendering works**: in `mode = sbs` the
game's render pass runs twice per game update (left eye, right eye), the first Present is swallowed
after its back buffer is stashed, and the second is composed into a **3840x1080 swapchain with
each eye a 1:1, unscaled 1920x1080 render at the engine's own 16:9 frustum**. Temporal state that
would cross eyes is kept **per eye** (section 5): TAA history pair and ping-pong index, previous
camera matrices (camera and object motion vectors), the TAA jitter phase (one per update, same for
both eyes) and the objects' previous transforms, so TAA and object motion blur stay on in sbs
(measured: the disparity field with TAA on matches the TAA-off one, no cross-eye blending). DLSS
gets a second NGX feature for the right eye, so each eye accumulates its own history (measured:
disparity field as with TAA off, sharpness as mono DLSS; the old reset-every-frame is the
fallback). **Both eyes now render inside one engine frame** by default (`sbs_path =
view`, section 6): `renderFrame` twice inside one `ModeSwitch::render`, beginFrame/endFrame, HUD and
Present once per update, the HUD duplicated into both halves, render-side state that a render
advances rewound for the second eye (separation-0 halves 0.036/255 apart), and optionally the
sky probe reused for the second eye (`skip_probe`; shadow-map reuse, +10 % fps, is opt-in until the flashlight issue in section 6 is solved). What
is not done: HUD depth. Convergence in full SBS
is a per-eye image shift in the compositor (section 6, "Convergence as image shift"), which TAA
cannot undo.

Every address is an RVA (image base 0x140000000), `Game_f_x64_EOS.exe` as shipped on Epic
(2026-11). "static" = read in Ghidra (`re/scripts/Serve.java`), "live" = observed with the
`stereo census` trace or captures in the lab, "measured" = numbers from captured frames.

---

## 1. Frame structure

### One thread does update, render submission and Present

The **"RMD - Game Thread"** (entry `0x347730`, names itself via `0x3c74c0`; reads
`CommandLine.txt` next to the exe; its loop calls `0x1d1900` once per iteration) runs:

```
0x1d1900  Game::tick
  0x263600  ModeSwitch::update     simulation: game modes, camera (setViewToWorld on the main view x4)
  0x262cb0  ModeSwitch::render     the frame:
              spin until >= 4.17 ms since the last frame (240 fps cap)
              0x2aca30  Renderer::beginFrame(g_renderer @0xb18848)   ++frame counter @0xb188d8
              mode->render() for each active mode (vtable +0x38), e.g.
                0x34cdd0  GameMode::render
                  0x2ae2f0  Renderer::renderView(view)  -> focus/DOF params, then tail-jumps to
                  0x508970  RendererX86::renderFrame(view, scene, extra)     [vtable slot 11]
                              TAA jitter -> View::setPostProjection, then
                  0x5090b0      RendererX86::renderWorld: RT pool (pColorRT, pNormalRT,
                                pLinearDepthRT, pMotionVectorsRT, pTAAHistoryRT0/1, ...),
                                shadows, cube-map probe (6 views), G-buffer, lighting, post
                  0x335550, 0x33b180  overlays / HUD
              0x2acb10  Renderer::endFrame   submits, then IDXGISwapChain::Present
```

Established **live** (census, `quiver lab call awr stereo census N`): every event below happens on
one thread id (the one that created the swapchain), in this order per frame:

```
setViewToWorld MAIN x4 (ret 0x15e723)          <- ModeSwitch::update
ModeSwitch::render enter
  ExecuteCommandLists n=1
  Renderer::beginFrame
  setViewToWorld MAIN x2
  renderFrame ret=0x34ce30 view=MAIN fov=1.3823 aspect=1.7778 near=0.2 far=1000
    setPostProjection (jitter)  ret 0x508de3
    setViewToWorld x6 cube faces (static PerspectiveView @0x140dc44f0, ret 0x5124b7)
    setViewToWorld CustomView @0x140e0b480 (ret 0x52e57a) = main camera pose
    setPostProjection x4 (0x5093e2 zero, 0x498602 identity, 0x498a1a / 0x509417 jitter)
    ExecuteCommandLists n=14..15                  <- the scene's command lists, submitted at the
                                                     end of renderFrame (inside it: the sbs census
                                                     marks it IN-RENDERFRAME)
  Renderer::endFrame
    ExecuteCommandLists n=8, n=1
    Present
    GetCurrentBackBufferIndex (ret 0x49b7e5)     <- the engine asks DXGI every frame
```

So there is **no separate render thread consuming a frame packet**: the game thread records D3D12
command lists during `renderFrame` (about 5 ms wall time per call in the prologue scene,
recording plus submission) and submits the first half of the scene's lists from inside
`renderFrame` (the device kick), the second half (lighting, post, the final image) together with
the HUD/overlay lists and the frame's end in `endFrame` (correction from the command-list census,
section 6). (`rend::CommandBufferExecuteThread` exists
in RTTI; with the options seen here all `ExecuteCommandLists` calls come from the game thread, through `0x503184`.) Debug
render options registered in `0x2b0580`: "Multi Thread Rendering" (u8 `0x955632`), "Split Command
Buffers" (`0x955635`), "Late Shadow Pass" (`0xd755d6`), "Combined Post Pass" (`0x95566a`), "VSync
Enabled" (`0x95566c`), "DLSS Mode".

This structure is what makes same-update stereo cheap: **simulation and rendering are separate
calls on the same thread, and `ModeSwitch::render` can be called twice without running the
simulation in between.**

### The camera: rend::View

`rend::View` (vtable `0x751bb8`, 0xcb0 bytes) → `rend::PerspectiveView` (vtable `0x751bd8`, 0xcc8)
→ game `BaseCamera` (vtable `0x752ad0`; the main view object is a BaseCamera). Matrices are
**doubles, row-vector** (`v' = v*M`, translation in the last row), static from the ctor
`0x469ab0`, copy `0x46a4f0` and the methods below:

| offset | field |
|---|---|
| +0x008 | post-projection double[4][4] (TAA jitter in [3][0..1]), set by `View::setPostProjection` `0x46c090` |
| +0x1e8 | world-to-view double[4][3] |
| +0x248 | view-to-world double[4][3]: rows right, up, forward, position |
| +0x2a8 / +0x328 | view-to-clip and inverse (lazy, `0x46c120`: `vtbl[2]` projection × post-projection) |
| +0x3a8 / +0x428 | world-to-clip and inverse (lazy, `0x46c500`) |
| +0x4a8 / +0x4a9 | "projection valid" / "world-to-clip valid" |
| +0x4b0.. | shader uniforms `g_mWorldToView`, `g_mViewToWorld`, `g_mViewToClip`, `g_mClipToView`, `g_mWorldToClip`, `g_mClipToWorld`, `g_vViewPoint`: each 1 header + 4 slots, slot = 1 + a per-thread index at TLS+0x30060 (`0x15e140`) |
| +0xcb0 | PerspectiveView: horizontal FOV (radians), +0xcb4 zoom, +0xcb8 aspect (w/h), +0xcbc near, +0xcc0 far |

`PerspectiveView` projection (`0x4637f0`): `m00 = 1/tan(fov/2)/zoom`, `m11 = m00*aspect`,
`m22 = f/(f-n)`, `m23 = 1`, `m32 = -n*f/(f-n)` (D3D left-handed). Measured main camera in the
prologue: fov 1.3823 rad (79.2 deg horizontal), aspect 1.7778, near 0.2, far 1000, so
`m00 = 1.2088`. Setters: aspect `0x463c30`, far `0x463c50`, fov `0x463c70`, near `0x463c90`,
zoom `0x463cb0` (each clears +0x4a8).

**`View::setViewToWorld(const double m[4][3])` at `0x15e2f0`** is where every camera transform
enters the renderer: it stores the matrix, inverts it into world-to-view, marks world-to-clip
dirty and writes `g_mWorldToView`, `g_mViewToWorld` and `g_vViewPoint`. Everything the GPU sees for
that view (and culling) is derived from it. Callers (static): `0x15e650` (float camera transform at
+0xcc8, used by BaseCamera: the main view, live), `0x477190` (spot-light shadow views),
`0x485390`, `0x50f5f0`, `0x512220` (the 6-face cube probe), `0x52e140` (a CustomView copied from
the main camera each frame).

How it reaches the GPU: the uniforms above are copied into constant buffers when the view is bound
(`UseViewCommand` `0x46cc10` / `UsePerspectiveViewCommand` `0x463cd0` → `0x46c890`); renderWorld
binds the main view three times (`0x506b20`).

### TAA and temporal state (static + live)

- TAA on/off: u8 `0x955636` (and `0x955694`), written by the options code `0x242a20` (forced 0
  when DLSS `0xd75620` is on). `renderFrame` only jitters when it is set.
- Jitter: `renderFrame` builds an identity post-projection with `[3][0..1]` = jitter / resolution
  (patterns Rotated Grid / Halton / Hammersley, tweakables "TAA:Jitter ..."), frame counter `0xd744f8`.
- `TAA::beginFrame` `0x498590` (`*(RendererX86+0x140)`): uses **TAA+0x60, last frame's unjittered
  world-to-clip (double[4][4])** to build current-view-to-previous-clip (camera motion vectors),
  then overwrites it with this frame's.
- History targets `pTAAHistoryRT0/1` and `TAA Mask 0/1` come from the RT pool by name in
  `renderWorld`; the ping-pong index is RendererX86+0x2e8 (section 5 has the details).

### The tweakable registry

`rend::BaseTweakable` ctor `0x461320(this, name, type)` appends `this` to a global array of at most
0x500 pointers at `0xd386f0`, count at `0xd3aef8`, under a mutex. Layout: +0x00 `char*` name,
+0xa8 type (0 bool, 1 float, 2/3 vectors, 5 list), +0xb0 value, +0xb4/+0xb8 float min/max. Most are
function-local statics, so a tweakable exists only once its code ran. Lookup by name is a linear
scan (the mod's `tweak_find`). Exposed live as `quiver lab call awr tweak list [SUBSTR]` and
`tweak NAME [VALUE]`. Examples present in-world: `Camera:FOV` (88, 15..150; registered `0xfaa0`),
`TAA:Resolve`, `TAA:Enable Camera Velocity`, `TAA:Enable Object Velocity`, `TAA:History Blend
Weight` (0.8), `TAA:Jitter Scale`, `Cinematic Moment:FOV Multiplier`. Cheap levers for stereo:
FOV and the TAA/velocity switches (used below).

Level-related (for the level-loading work): the game thread entry `0x347730` reads
`CommandLine.txt` from the exe folder and parses it with `0x195cf0` (errors: "Command line
errors"); `GameMode::loadWorld` exists (RTTI `InstanceWorldJob@?BE@??loadWorld@GameMode@@`); the
main menu has an "Episodes" item.

---

## 2. Camera bias (mono frame)

Hooks (`src/features/stereo/stereo.cpp`, signatures in `engine.h`):

- `RendererX86::renderFrame` `0x508970`: its `view` argument **is** the main scene camera; it is
  the only View passed there (live: one call per frame, always the BaseCamera). This scopes the
  bias: the mod biases that View only.
- `View::setViewToWorld` `0x15e2f0`: when the game sets the main view's transform, the mod keeps
  the game's matrix and forwards `position += right * offset`. Shadow, light, cube-probe and
  reflection views have their own transforms and never see the offset (the cube probe and the
  CustomView are set inside renderFrame; the CustomView copies the biased main pose, as it should).
- `View::setPostProjection` `0x46c090` (inside renderFrame, main view only): convergence, adds
  `offset * m00 / Zc` to `[3][0]` (an NDC shift that zeroes disparity at `Zc` metres).

Sign: view +x is camera **right** here (`right = up × forward`, left-handed, live census), `m00 > 0`;
the left eye is `-separation`. Measured: with parallel eyes every confident patch has the left
image to the right of the right image, the correct sign.

Because the offset goes into the view transform, everything derived from it moves together:
lighting, shadows (cast from light views, sampled at the biased camera's pixels), fog, SSAO, post.
In the captures shadows, light shafts and the HUD stay coherent; the HUD is drawn in screen space
and is identical in both eyes (zero disparity, i.e. at screen depth).

### Measurements

Scene: fixture **`prologue-road`** (Continue Game → the prologue road, the player standing, camera
behind him, lighthouse and trees in fog). 1920x1080, `m00 = 1.2088`,
`K = 2e*m00*(W/2) = 75.43 px*m` at 32.5 mm per eye. Tool: `re/scripts/stereo_disparity.py`
(32 px patches on a 24 px grid, SSD over integer shifts + parabolic sub-pixel, confidence-gated).

Mono-frame bias (separate left / right captures, run r10, TAA settled 60 frames after each change):

| pair | median | notes |
|---|---|---|
| mono control, consecutive frames | -0.016 px (p5..p95 -0.47..+0.17) | floor |
| player (2-3 m) at 32.5 mm | +29.4 px | implies 2.6 m |
| road in front (≈5 m) | +15.05 px | 5.0 m |
| trees through the fog (≈35-40 m) | +2.08 px | 36 m |

Near over far is 29.4 / 2.08 ≈ 14: not a pan. Disparity scales with separation (per-patch ratios,
patches > 3 px): 16.25 → 32.5 mm ×1.963 (requested 2), 16.25 → 65 mm ×3.890 (requested 4), 32.5 → 65
×1.961. Each eye here is a separate capture ~100 frames after the other, so idle animation and fog
motion between the captures add scatter and pull the medians 2-3 % low. The same-update battery
below removes that between the two eyes of a pair (not between pairs taken at different settings).

### Same-update battery (sbs, full SBS 3840x1080)

Every pair below is **the two halves of one presented 3840x1080 frame**, so both eyes come from
the same game update; no animation or camera drift between the eyes. Run r16, fixture
`prologue-road`, `re/scripts/stereo_battery.sh` + `re/scripts/stereo_analyse.py` (captures in the
lab run folder runs/r16). TAA and object velocity off (forced by sbs), per-eye camera motion vectors.

Control, **separation 0 in sbs** (two renders of the same update with the same camera):
disparity median **+0.009 px** (p5..p95 -0.23..+1.29, 2583 patches); pixel |L-R| mean 0.81/255,
max 14. The halves are not bit-identical: something rendered per pass differs slightly (film
grain/noise or a render-side simulation stepping per pass; the same measure on two consecutive
mono frames is 1.51/255). Bounding that is on the follow-up list.

Separation (per eye) | median of all patches | p95
---|---|---
16.25 mm | +2.62 px | +18.5 px
32.5 mm | +4.64 px | +34.3 px
65 mm | +8.96 px | +61.6 px

Field at 32.5 mm: 423 patches at 20-60 px (1.3-3.8 m: the player, the pole), 364 at 10-20 px
(3.8-7.5 m: the road), 435 at 5-10 px, 769 at 1-5 px (15-75 m: trees in the fog). Per-patch
scaling between captures (separate frames ~100 updates apart, so near moving things add scatter;
by disparity bin 2-5 / 5-10 / 10-20 / 20-60 px): 16.25 → 32.5 mm ×2.00 / 1.89 / 1.95 / 1.84
(requested 2), 16.25 → 65 mm ×3.85 / 3.37 / 3.58 / 3.05 (requested 4). The far bins, where nothing
moves, reproduce the request; the near bins are pulled down by the player's idle motion between
the separate captures. (The cleaner scaling check is the mono-frame table above, ×1.96 / ×3.89.)

**Convergence** 4 m vs parallel, both at 32.5 mm, joined per patch: parallel − converged =
**+19.34 px** median (expected a uniform K/Zc = 18.86 px), recovered **Zc = 3.90 m** for 4.00
configured (-2.5 %). With convergence, patches nearer than 4 m stay positive (median +4.34 px,
n=451) and farther ones cross to negative (median -10.17 px, n=1528); with parallel eyes 100 %
of patches are positive. A pan cannot change sign with depth.

**Same update vs frame-sequential, the measurement this design exists for.** A synthetic camera
yaw of 0.5 deg per game update (`stereo.test_pan`, applied to the main view once per
`ModeSwitch::render`) stands in for a player turning at 30-70 deg/s:

pair | far field (y < 500, trees in fog), median | p5..p95
---|---|---
sbs, halves of one frame (2 frames) | **+1.73 / +1.76 px** | -1.7..+8.8
alternate (frame-sequential), L(n) vs R(n+1) | **+12.69 / +12.35 px** | -3.8..+35.9
static sbs (no pan), for reference | +1.8..+2.1 px |

The frame-sequential pairs carry the camera motion between the two eyes: +10.6 px of false
disparity on the far field, which is what 0.5 deg is at the image centre
(960 × 1.2088 × tan 0.5° = 10.1 px). The same-update pairs carry none: their far-field disparity
is the static value. This is the Ishimura problem (`docs/49`, `19-stereo-motion`), removed by
construction rather than compensated.

![full SBS, both eyes from one update](img/stereo-sbs-full-3840.jpg)

*3840x1080 composite (downscaled), run r17: left eye in the left half, right eye in the right half,
each a 1:1 copy of the engine's 1920x1080 render. The HUD hint is drawn in both at zero disparity.*

![eye switch read as motion](img/stereo-eye-switch-motion-blur.jpg)

*Left: first sbs build (r11), the player smeared by motion blur because the eye offset read as
velocity (camera and object motion vectors from the other eye's previous frame). Right: with
per-eye previous world-to-clip and object velocity off (r17).*

### Census of an sbs update (live, r17)

```
setViewToWorld MAIN x4                      <- one ModeSwitch::update
ModeSwitch::render enter
  beginFrame; renderFrame(MAIN)             <- left eye: CustomView copy at (-3101.670, -931.870)
    ExecuteCommandLists n=15 IN-RENDERFRAME
  endFrame: ExecuteCommandLists n=8, n=1; Present   <- swallowed (back buffer index stays 1)
  [mod] ExecuteCommandLists n=1             <- stash copy, queued after the engine's lists
  beginFrame; renderFrame(MAIN)             <- right eye: (-3101.696, -931.810)
    ExecuteCommandLists n=15 IN-RENDERFRAME
  endFrame: ExecuteCommandLists n=8, n=1; Present   <- composed and presented
ModeSwitch::render leave
```

The two eye positions differ by 0.065 m along the camera's right axis (-0.391, 0, 0.920): the
configured 2 × 32.5 mm.

---

## 3. Two eyes from one simulated frame

### Candidates

1. **Re-run the frame's render call per eye** (implemented). `ModeSwitch::render` `0x262cb0` is the
   whole render+present of one game update and the simulation (`ModeSwitch::update` `0x263600`) is
   a separate call. The hook runs it twice: pass 1 with the left eye, pass 2 with the right eye.
2. Call `renderFrame` twice inside one `ModeSwitch::render`: **implemented, the default**
   (`sbs_path = view`, section 6). The premise here was wrong in one point: only the first half of
   a render's lists is submitted inside `renderFrame`; the half holding lighting, post and the
   final image is submitted later (section 6), so the copy has to be recorded into the engine's
   own list. (1) stays available as `sbs_path = frame`.
3. Hold the simulation every other frame (time-frozen alternate frames). Not needed: (1) gets the
   same guarantee without touching the simulation.

### How (1) works

- Present filter (`core/present`): on pass 1 the back buffer (the left eye) is copied to an eye
  texture on the game's own queue and **the real Present is skipped**. Safe because the engine asks
  `GetCurrentBackBufferIndex` after every Present (live), so it simply renders pass 2 into the same
  buffer. GPU order is right: our copy is queued after the engine's submissions for that frame and
  before pass 2's.
- On pass 2 the right eye is copied out and both eyes are written to the back buffer, which is then
  presented.
- Full SBS: with `[stereo] output = full` the swapchain is created **3840 wide** while
  `GetDesc`/`GetDesc1` keep reporting 1920. Live: the engine keeps rendering at its own resolution
  into the left 1920x1080 of the 3840 buffer (captured), so the compositor is two 1:1
  `CopyTextureRegion`s: each eye is exactly the engine's 1920x1080 render with its own 16:9
  frustum, unscaled. No projection change is needed at all. (The lab's 1920x1080 gamescope display
  shows only the left 1920 columns of the 3840 buffer, so `quiver lab shot` sees the left eye; the
  mod's own `stereo shots` read back the whole buffer.)
- Half SBS (`output = half`, 1920 swapchain): the prototype shows the centre half of each eye (a
  crop, not a squeeze); a proper half-SBS needs a 2:1 filtered squeeze (a small pixel/compute
  shader pass) and was not done.

### What breaks, and what the prototype does about it

| state | problem when eyes alternate renders | prototype | proper fix |
|---|---|---|---|
| TAA history (`pTAAHistoryRT0/1`, `TAA Mask 0/1`, index RendererX86+0x2e8) | eye N resolves against eye N-1's image: the disparity collapses (measured, section 5) | **done**: right eye gets its own pool RTs (renamed in the pool allocator) and each eye its own ping-pong index | — |
| camera motion vectors (TAA+0x60, vector blur +0x60) | the eye offset reads as camera velocity: motion blur smears the whole frame | **done**: per-eye copies swapped around each eye's renderFrame | — |
| object motion vectors (View+0x88/+0x108 previous camera; object +0xb8 previous world) | the eye offset reads as object velocity: the player smears (captured); the second eye had no object motion at all (measured) | **done**: per-eye previous View matrices; objects keep the previous update's transform for the second eye | skinned animation (vertex buffers double-buffered by frame parity) still zero for the second eye |
| DLSS (`0xd75620`, one NGX feature at device+0x38a8) | one history for both eyes: the disparity collapses (measured) | **done**: a second feature for the right eye, swapped into the device per eye (section 5); reset every frame as the fallback | — |
| frame counters (`0xb188d8`, `0xd744f8`) | advance twice per update: jitter sequence and anything keyed on frame parity runs at 2x | jitter: renderFrame is lent a per-update counter (same jitter for both eyes); the rest unchanged | pin per update once beginFrame runs once per update |
| auto exposure, GPU particles, cloth, foliage repulsion (render-side sims) | step per render, so twice per update and possibly differently per eye | the separation-0 residual (0.81/255) was film grain (random offset per render), the motion-blur/TAA frame-time rings and lens flares | **done in the view path** (section 6): rewound for the second eye, residual 0.036/255 |
| HUD | drawn per eye at zero disparity | fine | **view path**: drawn once, duplicated into both halves (section 6) |
| frame pacing | ModeSwitch::render waits 4.17 ms per call | fine | — |

### Cost (measured, run r17, `quiver lab gpu -- re/scripts/stereo_rate.sh awr`, GPU to itself)

mode | renders/s | displayed frames/s
---|---|---
off (mono) | 140.8 | 140.8
sbs | 140.8-145.5 | 70.2-72.6

Exactly two renders per displayed frame and no other overhead visible at this precision (the
compositor is 3-4 copies on the copy path). On the user's 8 GB laptop GPU this means half the mono
frame rate, as expected for true stereo; the pass-2 shortcuts in section 4 are where it comes back.

---

## 4. Recommended architecture for full SBS 3840x1080

1. **Output**: a 3840x1080 swapchain created by the mod (done: `present_set_width_scale(2)`), the
   engine left at 1920x1080 internal resolution, each eye copied 1:1 into its half. The per-eye
   frustum is then exactly the engine's own 16:9 projection: no FOV or aspect change, unlike
   Ishimura's anamorphic route. The window is made 3840x1080 (2x the engine's size) with the
   engine kept at one eye, menus usable with the mouse over either half: done, see
   [stereo-output.md](stereo-output.md).
2. **Dual render per update**: done both ways; the default is `renderFrame` ×2 inside one
   `ModeSwitch::render` (section 6), the `ModeSwitch::render` ×2 structure is the fallback.
3. **Temporal state per eye**: done for TAA (history pair, index, jitter), camera and object
   motion vectors, DLSS (section 5). Remaining: skinned-animation velocity for the
   second eye, and an audit of render-side simulations (particles, exposure, SSAO and volumetric
   temporal buffers) to step once per update.
4. **Pass-2 shortcut**: done for the shadow pass and the sky probe (section 6, ~10 %); render-side
   state the second eye would step again is rewound (flares, grain, frame-time rings).
5. **HUD**: drawn once and duplicated into both halves at zero disparity (section 6); a chosen
   depth would shift the duplicate.
6. **Comfort defaults**: separation 32.5 mm per eye with convergence 3-5 m for third person,
   measured on the glasses.

### Risks

- Unknown render-side state stepped per render (particles, cloth, exposure) can differ between
  the two eyes of a pair (retinal rivalry). Detectable with the sbs separation-0 control.
- Cinematics/cutscenes use other cameras and may bypass `renderFrame` (Bink videos do: no 3D).
- The width lie is only on the DXGI side; the back-buffer resource itself reports 3840 (the engine
  did not use it in the tested paths: menu, title, gameplay).

---

## 5. Temporal state per eye

Implemented in `src/features/stereo/eyestate.{h,cpp}`: `eyestate_enter(renderer, view, eye, …)` /
`eyestate_leave(…)` bracket each eye's `RendererX86::renderFrame` (called from `render_eye`, so it
does not care whether the two eyes come from two `ModeSwitch::render` calls or two `renderFrame`
calls in one), and `eyestate_update(dual)` runs once per game update. New RVAs are in `engine.h`
(`kEyeSigs`, all signature-checked; a mismatch disables the whole module and sbs falls back to TAA
and object velocity off, as before).

### What carries "last frame", and what each eye now gets

**TAA history (static + live).** `renderWorld` (0x5090b0) requests `pTAAHistoryRT0/1` (output
resolution, format 0xb) and `TAA Mask 0/1` (format 3) from the RT pool every frame and keeps them in
RendererX86+0x2f0/+0x2f8 and +0x300/+0x308 for the frame (cleared at its end). They persist because
the pool allocator (0x4ad760) hands back the free entry whose stored name *contains* the requested
name (case-insensitive substring, 0x3bb360) with the same size/format. The ping-pong index is
RendererX86+0x2e8 (int): while TAA is on and DLSS off, renderWorld does `prev = i; i ^= 1;` then
`0x49a8c0(TAA, …, mask[i])` and the resolve `0x498a80(TAA, color, motion, …, mask[prev], mask[i],
hist[prev], hist[i])` (code at 0x50aa5d). With two renders per update the index toggles twice, so
eye R always reads what eye L wrote that same update (and vice versa).
Per eye: the pool allocator is hooked and, while the right eye renders, those four names (matched
by the game's literal addresses 0x798858/68/78/88) become `TB R TAAHistory0/1` and `TB R TAAMask0/1`
(no name is a substring of another, so the pool can never hand one eye the other's target): the
right eye gets its own persistent pair, created by the engine's own pool on first use (+2 history
RTs at output resolution, +2 masks; about 37 MB at 1920x1080). The index +0x2e8 is saved/restored
per eye. Live: the probe (`stereo probe N`) shows each eye's index toggling on its own (L 1→0, R
1→0, L 0→1, R 0→1 …) and 4 renamed requests per right-eye render.

**Jitter (static + live).** `renderFrame` (0x508970) derives the jitter phase from the render-frame
counter 0xd744f8 (`counter & 3` for the rotated grid, `counter % samples` for Halton/Hammersley,
also the DLSS phase), and the renderer's beginFrame (0x5064a0) increments it once per
`ModeSwitch::render`, so with sbs L always got even phases and R odd ones: each eye's history only
ever saw half the pattern. Per eye: the counter is replaced by a per-update count for the jitter
computation only (set in `eyestate_enter`, restored at entry to the hooked `renderWorld`), so both
eyes of an update use the same jitter and each eye walks the whole sequence, one step per update.
Everything inside renderWorld (skinning buffers, per-object stamps, GPU ring slots keyed on
`counter % n`) still sees the engine's real counter. Live: probe `ctr=114/115` at renderFrame
entry for both eyes of consecutive updates, real counter 2290.. inside renderWorld.

**Camera motion vectors (static + live).** Two objects keep last frame's unjittered world-to-clip
at +0x60 and overwrite it every render: the TAA object (RendererX86+0x140, `TAA::beginFrame`
0x498590) and the vector-blur object (RendererX86+0x190, 0x4700f0, same layout). Both are
saved/restored per eye (the TAA one was already in the prototype; the vector-blur one is new).

**Object motion vectors (static + live).** The velocity builder (0x4cad50, `RenderVelocityCommand`
0x4c9c90) computes `g_mPreviousLocalToClip = objPrevWorld(object+0xb8, float 4x3) × View+0x108
(previous world-to-view, double 4x3) × View+0x88 (previous view-to-clip, double 4x4)`, and the
current one from object+0x88 × View+0x1e8 × View+0x2a8. Live (probe): after renderFrame
View+0x108 equals the current world-to-view, i.e. the engine copies current → previous at the end of
each render, so the second eye's "previous camera" was the first eye's (the eye offset read as
object velocity: the player smear). Per eye: View+0x88..+0x168 is saved/restored per eye like the
TAA matrices. The objects' own previous world is the other half: `rend::Object::storePrevious`
(vtable 0x785d48 slot 6, 0x46d510: +0xb8 ← +0x88) is called for every drawn object at the very end
of renderWorld (0x50b46e). Live: ~330 calls per render in prologue-road, ~140 in forest-night, all
inside renderFrame (return address 0x50b471), none outside.
So with two renders per update the second eye found prev == current and drew **no object motion at
all**. Measured in `awr-forest-night` (birds attacking), objects drawn with a non-zero object motion
per eye render (status `obj_moving`): without the fix L 25-32, R 0.0-0.2; with it L 18.7-32.4, R
18.7-33.6 (three alternations each). The fix: while the first eye of an update renders (another
eye follows), the hook keeps +0xb8..+0xe8 as it was across the call (the rest of storePrevious
still runs), so both eyes see the previous *update's* transform. Object transforms are world-space
and eye-independent, so this is exact for rigid objects.

**Not per eye (remaining).** Skinned vertices are computed into two buffers chosen by
`counter & 1` (0x46d650 current, 0x46d680 previous, stamp check 0x46d6f0, static). With the counter
advancing per render, the right eye computes the same pose into the other buffer and uses the left
eye's as "previous": its skinned *animation* velocity is zero (rigid motion and camera are right).
This resolves itself once beginFrame runs once per update (both eyes on the same counter: the
second eye reuses the first's buffer, previous = last update's). The SSAO temporal buffer
(0x5363c0) and the volumetric-light temporal buffer (0x4e3020) are also single-history and still
blend across eyes (low-frequency effects; not measured separately).

### DLSS

Static: DLSS mode is u32 0xd75620 (renderWorld skips the TAA path when non-zero). One NGX feature
lives in the D3D12 device object (global 0xda35e0, `rend::DeviceD3D12PC` vtable 0x790f58): handle at
+0x38a8, parameters at +0x38a0, initialised flag +0x38b8, created by 0x4a7080 (NGX CreateFeature on
a command list kept at +0x38d0) from vtable slot 45 (0x4a6660), which re-creates it when the mode
(+0x38d8) or render/output size (+0x38c0..+0x38cc) changes, moving the old handle to +0x38b0 for
release by 0x4a6cd0 (GPU idle 0x4f7be0, then NGX release 0x68ea80). Evaluate is slot 46
(0x4a67b0 → parameter setup 0x4a5800); its **Reset is always 0** (`mov dword [rsp+0x68],0` at
0x4a6ad1). Live: DLSS does run in the lab (`game.dlss = quality` → mode 4, a feature handle
present; nvapi enabled in the instance).

What the mod does (`src/features/stereo/dlss.cpp`): **one DLSS feature per eye**. Static
(Ghidra, re-read for this): the feature handle is the only per-feature state. The evaluate's
parameter helper 0x4a5800 (NGX's `EVALUATE_DLSS_EXT`) sets every input on the shared parameter block
+0x38a0 on every call: color, output (RendererX86+0x270, written in full), depth, motion vectors,
exposure (the auto-exposure ring, 0x480320), jitter (RendererX86+0x358/+0x35c, the same for both
eyes of an update since section 5), MV scale (1, 1), sharpness (+0x38bc, from the optimal-settings
query, eye-independent), Reset (0), render subrect (the render-size globals 0xd745e8/ec). Slot 45 is
called at the start of **every** `renderWorld` (0x509150 → 0x50d7a0, which also handles the render
resolution and, when slot 45 reports a change, frees the renderer's size-dependent RTs and calls
slot 56 / slot 55 three times). Its decision uses the device "slot": handle +0x38a8, key
+0x38c0..+0x38cc (render w/h, output w/h), mode +0x38d8, against the globals (mode 0xd75620, output
size 0xd74600/04, render size 0xd745e8/ec). The device is created once (0x513e70 → NGX init
0x4a4900) and NGX shut down only in its destructor (0x4a50c0), so a handle stays valid for the
process. The game's own DLSS option change only sets +0x3739 (slot 109, 0x4fab30), consumed by
slot 56; the feature itself is re-created by slot 45 in the next `renderWorld`.

So the right eye's feature is a second copy of that slot:

- `eyestate_enter` for the right eye (sbs, both eyes in this update, DLSS on, NGX initialised, the
  engine has its own feature): the left eye's slot (handle, key, mode) is saved and the right eye's
  is written into the device; the engine's **own slot 45** is then called once on it, with +0x38b0
  emptied for the call. On first use (slot all zero) or after a mode/size change it runs the
  engine's optimal-settings query (0x4a73e0) and CreateFeature (0x4a7080, recorded on the device's
  own creation list +0x38d0 and submitted with the frame's lists), exactly as for the engine's
  feature; otherwise it returns 0. The `renderWorld` that follows then finds the slot current, and
  its evaluate uses the right eye's handle.
- `eyestate_leave` for the right eye: the right slot is read back (normally unchanged) and the left
  slot restored, so the engine's own feature and key never see the right eye's.
- An old right-eye handle (mode or size change, DLSS off, `dlss_per_eye = 0`, sbs off) is never
  given to the engine's +0x38b0. It is released (NGX ReleaseFeature 0x68ea80) once a fence
  signalled on the game's queue at that point has completed (its last evaluate was in an earlier,
  already submitted frame); live, 10-50 ms later, every release returned 1 (success). A first
  version called the engine's GPU idle 0x4f7be0 between the eyes, as slot 56 does: a 5 s stall
  on a live DLSS mode change, then an access violation in 0x4f7640 (a command-list wrapper with
  index −1, reached from 0x4b7370 in the right eye's `renderWorld`; game minidump, run r1). The
  idle submits the device's queued lists (0x5034c0), apparently including the thread's open one,
  which the right eye then kept recording into.
- Fallback, as before: the evaluate's Reset immediate (0x4a6ad5) is patched to 1 while sbs runs with
  DLSS on and per-eye DLSS is unavailable: signature mismatch (slot 45, ReleaseFeature,
  CreateFeature prologues), `stereo.dlss_per_eye = 0`, or a failed creation (then until the
  engine's key changes or sbs restarts; that eye shares the engine's feature meanwhile).
- With `mode = off` (or menus, or alternate) nothing is swapped and nothing patched: live, `stereo
  status` `dlss.swaps` constant over ~280 presents, `right` null, `reset` false.

Cost: one more DLSS feature (its internal resources; VRAM not measured, the GPU was shared) and per
update one call of slot 45 that only compares the key, plus two 28-byte slot copies. Alternate mode still shares one feature (unchanged; it never had per-eye DLSS).

### Measured: per-eye DLSS (lab instance awr-dlss)

`re/scripts/stereo_dlss_battery.sh` (the `stereo_taa_battery.sh` captures per variant: separation 0
and 32.5 mm static, the same under `test_pan 0.5`, plus a mono capture with `mode = off`); each cell is the two halves of one
presented frame, 2 frames per cell; disparity per patch with `stereo_taa_analyse.py` against the
TAA-off variant of the same run (**ref**: `stereo.taa = 0`, DLSS off). **per eye** = this, **reset**
= `stereo.dlss_per_eye = 0` (the old behaviour). Sharpness = mean |∇| over a static box of
vegetation/road edge (eye x 100-700, y 500-1000; hsbs x 50-350), whole frame in brackets.
prologue-road. Full output: run r4 (windowed 1920x1080 per eye, 3840x1080 swapchain). hsbs: run r5
(borderless 1920x1080 desktop, 960x1080 per eye, anamorphic). The GPU was shared with another
instance; nothing here is a timing.

full, 1920x1080 per eye (Quality renders 1280x720, Performance 960x540):

| | ref (TAA off) | DLSS Q per eye | DLSS Q reset | DLSS P per eye | DLSS P reset |
|---|---|---|---|---|---|
| sep 0, static: \|L−R\| mean (/255) | 0.040-0.042 (max 7) | 0.088-0.090 (max 5) | 0.049-0.064 (max 6) | 0.134-0.135 (max 30) | 0.297-0.314 (max 53) |
| sep 0, pan: \|L−R\| mean | 0.24-0.28 (max 13) | 0.29-0.32 (max 11) | 0.24-0.30 (max 14) | 0.44-0.47 (max 22) | 0.35-0.42 (max 38) |
| sep 32.5: per patch vs ref, median / mean \|diff\| | (ref vs ref frame: medians +3.82 / +3.30) | **+0.016 / 1.83, −0.000 / 1.56 px** | −0.000 / 1.98, −0.022 / 1.94 px | **+0.025 / 1.64, +0.019 / 1.61 px** | +0.042 / 2.22, 0.000 / 2.00 px |
| sharpness, static box (whole) | 3.19-3.27 (3.60-3.67) | **2.45-2.49** (2.43-2.46) | 1.88-1.91 (2.26-2.27) | **3.18-3.28** (3.50-3.56) | 2.61-2.66 (2.78-2.95) |
| same, mono (`mode = off`) | | 2.42-2.44 (2.44-2.47) | | 3.41-3.42 (3.55) | |

hsbs, 960x1080 per eye (Quality 640x720, Performance 480x540; right-eye feature created for
output 960x1080, live log):

| | ref (TAA off) | DLSS Q per eye | DLSS Q reset | DLSS P per eye | DLSS P reset |
|---|---|---|---|---|---|
| sep 0, static: \|L−R\| mean | 0.048-0.050 (max 5) | 0.079 (max 5) | 0.15 (max 8)* | 0.085-0.107 (max 12) | 0.28-0.29 (max 50) |
| sep 0, pan: \|L−R\| mean | 0.35-0.37 (max 10) | 0.17-0.19 (max 5) | 0.22-0.24 (max 6) | 0.28-0.30 (max 29) | 0.39-0.41 (max 49) |
| sep 32.5: per patch vs ref, median / mean \|diff\| | | **−0.019 / 0.80, −0.000 / 0.79 px** | −0.020 / 0.92, −0.044 / 0.93 px | **+0.007 / 0.49, +0.001 / 0.50 px** | −0.013 / 0.96, −0.060 / 0.93 px |
| sharpness, static box (whole) | 2.87-2.92 (3.67-3.70) | **2.50-2.69** (2.64-2.81) | 1.91-1.96 (2.61-2.63) | **3.31-3.33** (3.70-3.73) | 2.53-2.64 (3.17-3.26) |
| same, mono | 2.65-2.75 | 2.54-2.81 (2.74-2.95) | | 3.23-3.38 (3.74-3.87) | |

\* second frame 1.33 (max 138): a lens flare differing between the halves in that frame, not DLSS.

Reading it:

- **Disparity** (a): per eye, the field matches the TAA-off one patch by patch (median difference
  within ±0.03 px; the mean |diff| of 0.5-1.8 px is the upscaler's own antialiasing moving edges and
  the scene's idle motion between captures, as for TAA in section 5: 0.84-0.91 px in r2). With one
  shared history (r11, above) it was −2.4 px median.
- **No cross-eye blending** (b, d): at separation 0 both eyes are equal to within 0.08-0.13/255
  static and 0.17-0.47/255 under the pan, the same order as the TAA-off reference (0.04 / 0.24-0.37);
  the shared history measured 1.21 / 0.90 (max 77-83) under the pan in r11. Crops of the pan
  frames (prologue-road's car and fence) show no ghost or smear on either eye.
- **Sharpness** (c): per eye equals mono DLSS (Quality 2.45-2.49 vs 2.42-2.44, Performance
  3.18-3.28 vs 3.41-3.42 in full; the same in hsbs) and is clearly above the reset fallback:
  +30 % (Quality) / +22 % (Performance) on the static box in full, +31 % / +28 % in hsbs. The
  whole-frame number moves less (fog and sky dominate it). The game's DLSS Performance measures crisper
  than Quality, in mono too: 0x4a7080 creates Quality with render preset hint 6 (F) and
  PerfQuality 2, Performance with hint 13 and PerfQuality 0 (static), i.e. a different model.
- **Live switching** (f), 5 + 5 + 10 rounds in full and hsbs (`churn`: sbs off/on, DLSS
  Performance, off, Quality, `dlss_per_eye` 0/1): no crash, no stall (stall watchdog silent), no
  vkd3d.log output; every right-eye feature created was released (r5: 43 created, 43 released after
  the final `mode = off`, every NGX ReleaseFeature returned 1 = success); the frame path
  (`sbs_path = frame`) works the same way (r5, created once, swapped per update).
- **awr-forest-night** (hsbs, run r15; a scripted bird attack, so nothing is static): DLSS Quality
  per eye vs the TAA-off field per patch −0.001 / −0.005 and +0.006 / +0.005 px median (mean
  |diff| 0.34-0.52 px; two pairs of captures), reset −0.017 / −0.024 px; static-box sharpness per
  eye 3.09-3.43 vs reset 2.98-3.07 (whole frame 3.05-3.19 vs 2.90-2.97); separation 0 |L−R|
  0.16-0.35/255 per eye, 0.77-0.79 reset, 0.56-0.64 TAA off (the flock differs between the eyes,
  section 6). This fixture sometimes ends the game: this branch 3 exits in 8 launches (two
  during loading, before any eye render; one 12 s in-world, a C++ exception thrown from the
  engine's allocator path, 0x3c2a4a, minidump), master without this change 1 in 3 (in-world after
  10 s, the same exception and stack; A/B with the same instance, fixture and settings). Not
  caused by per-eye DLSS; the "crash in forest-night" of section 6's open issues.

![DLSS per eye vs reset](img/stereo-dlss-per-eye.jpg)

*Right eye (left column: mono), static vegetation at the road edge, prologue-road, full output, r4,
brightened 2.2x: TAA off reference; DLSS Quality mono; DLSS Quality per eye in sbs; reset every
frame (soft: the grass blades smear); DLSS Performance per eye; reset.*

### Measured (lab, `prologue-road`, 3840x1080 sbs, separation 32.5 mm, parallel)

`re/scripts/stereo_taa_battery.sh` + `re/scripts/stereo_taa_analyse.py`; every number is the two
halves of one presented frame (both eyes from one update), 2 frames per cell. Variants: **off** =
TAA and object velocity off (the old sbs default; the reference disparity field), **shared** = TAA
and object velocity on with per-eye state off (`stereo.per_eye = 0`, the old `taa = 1`), **per-eye**
= the new default, **mb-** = TAA off, object velocity (motion blur) on. Runs r4 (TAA) and r11 (DLSS
quality, then the off / per-eye repeat with DLSS off; before per-eye DLSS existed).

| | off | shared | per-eye |
|---|---|---|---|
| sep 0, static: \|L−R\| mean (/255) | 0.78, 0.56 | 0.87, 0.70 (max 50-64) | 0.80, 0.54 |
| sep 0, pan 0.5°/update: \|L−R\| mean | 0.91, 0.75 (max 16) | 1.04, 0.97 (max 80-82) | 0.80, 0.74 (max 23-31) |
| … player box | 0.61, 0.63 | 1.51, 1.36 | 0.69, 0.56 |
| sep 32.5: disparity median (all patches) | +4.03, +3.99 px | **+0.85, +1.19 px** | +4.05, +4.07 px |
| … per patch vs off: median, mean \|diff\| | (off vs off: −0.002, 0.27) | −1.52, 4.51 px (p5 −15.8) | **−0.012, 1.24 / 1.20 px** |
| … sharpness, mean \|∇\| (player box) | 3.66, 3.49 (5.67, 5.55) | 2.84, 2.73 (4.25, 4.15) | 3.61, 3.55 (5.61, 5.51) |

With a shared history the right eye's TAA keeps blending in the left eye's image (and vice
versa): the disparity field collapses towards zero (median +0.85 px instead of +4.0: the 3D effect
is mostly gone) and the image is soft. With per-eye state the disparity field matches TAA-off patch
by patch (median difference −0.01 px; the 1.2 px mean is TAA's own antialiasing moving edges,
against a 0.27 px floor between two TAA-off frames), the separation-0 controls are at the TAA-off
level both static and under the pan, and sharpness is back to the TAA-off level. Repeat in r11:
per-eye vs off per patch −0.001 px median, 1.07 / 0.99 px mean.

DLSS (r11, DLSS quality, same scene): **shared** (no handling) disparity median +0.25 px (far field
+0.15 px vs +2.7 px), per patch vs off −2.4 px median, 5.1 px mean, pan |L−R| 1.21 / 0.90 (max
77-83); **reset per frame** (then the mod's handling, now the fallback): disparity +3.87 / +3.68 px, per patch −0.03 px
median, 1.48 / 1.45 px mean, pan |L−R| 0.79 / 0.69 (max 13-35); sharpness 2.16 / 2.06 (soft: no
accumulation).

Object velocity on, TAA off (the r11/r17 smear case): player-box sharpness L 5.14 / 5.02 with the
previous view shared, 5.41 / 5.31 per eye, 5.67 / 5.55 with object velocity off. The crops show the
smear on the shared variant's head and shoulders; in the per-eye capture the player had moved (idle
animation), with ordinary motion blur on the moving arm. A moving-objects |L−R| test in
`awr-forest-night` (birds) was inconclusive: frame-to-frame variation (0.8-2.1/255) is larger than
any variant difference; the `obj_moving` counts above are the clean measurement there.

Cost (`quiver lab gpu -- stereo_rate.sh`, awr-pass also resident): sbs with per-eye state 68.5 /
68.2 renders/s, without 68.1 / 68.0: no measurable overhead (a few memcpys per eye; the right
eye's extra RTs are only memory).

![TAA and DLSS per eye](img/stereo-taa-per-eye.jpg)

*Right eye, player crop, separation 32.5 mm (r4, r11). TAA with one history for both eyes blends
the left eye's image in (soft, doubled edges, disparity flattened); per-eye history looks like the
TAA-off frame, antialiased. DLSS with its single history: the same blending; reset per frame: no
blending, softer.*

![object velocity per eye](img/stereo-object-velocity-per-eye.jpg)

*Right eye, TAA off, object motion blur on (r4): previous camera shared between eyes (the eye
offset reads as object motion: the head and shoulders smear) vs per eye.*

### Config

`[stereo] taa = 1` and `object_velocity = 1` (new defaults) keep the game's own TAA / object
velocity settings in sbs, with per-eye state; `0` switches them off while sbs runs (the old
behaviour). Without per-eye state (signature mismatch) they are switched off regardless. Dev:
`stereo.per_eye = 0` (shared state, the old prototype), `2` (all but the object-previous hold); the
lab verb `stereo probe N` logs the view/TAA state around N eye renders; `stereo status` reports
`eyestate` (swaps, renamed RT requests, jitter count, held object stores, `obj_moving` per eye,
and `dlss`: mode, left/right handles, the right feature's key, creates/releases/pending, reset).
`stereo.dlss_per_eye = 0` switches per-eye DLSS off (one feature, reset every frame).

---

## 6. Both eyes in one engine frame (`sbs_path = view`, the default)

Code: `hk_modeSwitchRender` / `render_view_sbs` in `stereo.cpp`, `cmdlist.{h,cpp}` (D3D12
command-list layer), `gpu_stash_mid` / `gpu_place_mid` in `gpu.cpp`, `pass2.{h,cpp}` (second-eye
handling). `sbs_path = frame` keeps the `ModeSwitch::render` ×2 path of section 3.

### How the engine submits a render (live, `stereo census N cl`)

`census N cl` also traces the command-list calls that touch the back buffer (barriers, copies,
`OMSetRenderTargets` with the RTV/DSV handles, list resets/closes with draw counts, the lists in
each `ExecuteCommandLists`). Mono frame, prologue-road:

```
ModeSwitch::render enter
  ~23 command lists Reset (a per-frame pool, handed out in order)
  ExecuteCommandLists n=1                       <- frame-start list: barrier bb PRESENT -> RENDER_TARGET
  beginFrame; renderFrame
    ExecuteCommandLists n=14..15 IN-RENDERFRAME <- the device "kick": every list closed so far
                                                   (shadows, probe, depth/G-buffer: ~4200 draws)
    list X: OMSetRenderTargets(back buffer, no DSV), 1 draw   <- the final post pass, recorded but
                                                                 NOT submitted
  renderFrame leave
  endFrame
    lists closed: 61, 2822, 2237, 200, 200, 200, 54 draws, list X (bb_draws 1 + HUD draws)
    ExecuteCommandLists n=8                     <- lighting/forward/post + the final image + HUD
    ExecuteCommandLists n=1                     <- barrier bb RENDER_TARGET -> PRESENT
  Present
```

So, unlike what section 3 assumed, a render's lists go out in **two batches**: the kick inside
`renderWorld` (the device's vtable +0x378, `0x4f6280` → `0x4f62b0` → `0x5034c0` →
`ExecuteCommandLists` at `0x503184`; called once per render from the main pass `0x506b20`, before or
after the shadow pass depending on "Late Shadow Pass"), and `endFrame`. The second batch holds
about 5700 of ~10000 draws and the final post pass into the back buffer (static: `renderWorld`
`0x5090b0` ends with `0x532030(post, scene, target)`, the target being the device's back-buffer RT
from `device->vtbl[+0x48]`; live: the `OMSetRenderTargets` on the back buffer is after the kick).
The HUD (`GameMode::render` → `0x335550`/`0x33b180` and the other modes' `render`) records into the
back buffer's list with the scene DSV bound (live: `ds=0x...` on its `OMSetRenderTargets`).

### What the view path does per update

```
ModeSwitch::render (once)          beginFrame (once: counter 0xb188d8, RendererX86 beginFrame
                                   0x5064a0 -> render counter 0xd744f8 and frame time 0xd74504)
  renderFrame hook:
    pass2_eye(0); render_eye(LEFT)                  (eyestate: per-eye TAA/velocity, jitter)
    gpu_stash_mid: barrier bb RT->COPY_SRC, copy bb[0,1920) -> eye texture 0, barrier back,
                   RECORDED INTO THE ENGINE'S OPEN BACK-BUFFER LIST (after its post pass)
    engine kick (device vtbl +0x378)               -> the left eye's second batch goes out now
    pass2_eye(1); render_eye(RIGHT)                 (rewinds; optional shadow/probe reuse)
    gpu_place_mid (also recorded into the engine's open back-buffer list):
                   bb[0,1920) -> eye texture 1; eye 0 -> bb[0,1920); eye 1 -> bb[1920,3840)
    overlay duplication armed
  HUD / other modes: each draw into the back buffer is issued twice (viewport and scissor
                   +1920 px, DSV unbound for the copy since rasterisation is limited to the
                   smallest bound target, the 1920-wide depth buffer)
endFrame (once): ExecuteCommandLists n=8, n=1; Present (once, nothing swallowed)
```

Why the copies are recorded into the engine's list: a list of ours submitted when `renderFrame`
returns runs *before* the eye's second batch (live, first attempt: the stash captured the back
buffer before the post pass). Recording into the list that last bound the back buffer (tracked by
the `OMSetRenderTargets` hook, still open at that point) puts the copy right after the post pass in
GPU order; with the kick the list is closed and submitted immediately after. The back buffer's
state at that point is whatever the engine's barriers left it in (tracked per back buffer from
`ResourceBarrier`, record time: `RENDER_TARGET`); each copy transitions from and back to it.
Live census of a view-path update (r6/r13): the stash copy and its barriers inside the left eye's
back-buffer list, `ExecuteCommandLists` of that list from the kick, the right eye's lists, the
placement copies inside the right eye's back-buffer list, then the HUD's draws with `dups=4`.

Why the kick matters (measured, GPU shared with another instance, so GPU-bound): without it the left
eye's second batch waited for the right eye's kick, the GPU idled meanwhile, and the view path ran
61 renders/s against 70 for the frame path; with it 69.1-69.4 for both.

Frame-level work now once per update: beginFrame/endFrame (screenshot/scaling bookkeeping, the
`0xb188d8` counter), the HUD and overlays, the frame-start/end barrier lists, Present, the
renderer's beginFrame (`0x5064a0`: render counter, frame time, per-frame resource rings). Both eyes
see the same render counter, so per-object "once per frame" updates (stamps compared with
`0xd744f8`: skinning `0x46dff0`/`0x4b2390`, `0x46ea00`) run in the first eye only and the second
eye reuses them, including the skinned vertex buffers double-buffered by counter parity
(`0x46d650/0x46d680`; section 5): with both eyes on one counter value the second eye uses the
first eye's buffer and the previous one is last update's, i.e. skinned animation velocity is
right in both eyes. GPU rings keyed on `counter % n` (auto exposure `0x480340`/`0x480bf0`: a ring of
1x1 exposure RTs and readback buffers; `0x4b5a60`/`0x4b6950`): the second eye reads the same
previous slot and writes the same current slot as the first, in GPU order after it, so the result
is the second eye's (equal to the first's within the luminance difference between the eyes); the
CPU readback of a slot happens `n` frames later (static). No overwrite hazard: one queue, in order.

### Second eye: state a render advances (`pass2.cpp`, measured)

Separation 0 in the view path should give identical halves (same update, same camera). It did not:
mean |L−R| 3.88/255 (r6), 0.95 after the flare fix. Causes found, each verified by switching it:

- **Lens flares** (`rend::Flare` vtable `0x78f768` slot 8, `0x492bb0`, static): per render the flare
  pushes its occlusion query result into a 10-sample average and resets the average unless it last
  ran on frame `counter − 1` (+0x130). Twice in one frame the second call resets it: no flares in
  the second eye (captured: the street lamp's flare only in one half). Fix: the flare's state
  (+0xd0 index, +0xd4 count, the ring at +0xe0, +0xe8/+0xec fade, +0x130) is snapshotted before the
  first eye's call and put back before the second eye's (`stereo.flare_rewind`).
- **Film grain** (static): the post object (RendererX86+0x118) draws `g_vRandomOffset` for the grain
  from a Mersenne Twister at +0xa8 (624 words + index at +0x9c0, `0x3e64e0`) every render, so the
  two eyes got different grain. With grain off in the game's options the residual fell from 0.95 to
  0.10/255 (r6).
- **Frame-time rings** of motion blur (`0x470750`: globals `0x955618` index, `0x95561c` count,
  `0x955620` capacity, `0x955628` float*) and TAA (`0x49a8c0`: `0x955850..0x955860`): every render
  pushes the frame time, so the second eye averaged one more sample. Motion blur off: 0.10 → 0.035.

The generator and both rings are snapshotted when the first eye starts and put back when the second
starts (`stereo.rewind`); a ring is restored only if its array pointer is unchanged (it is
allocated lazily on first use, which can happen during the first eye: live crash when the whole
header was rewound right after motion blur was switched on). Result (r7, grain and motion blur on,
TAA off): 0.052-0.111/255 (max 3) with, 0.56/255 without. Remaining residual: the volumetric light
shafts (temporal buffer `0x4e3020`, single history across eyes, section 5). With master's per-eye
TAA: **0.036-0.038/255** (max 6, r16 battery) and 0.001-0.012/255 in `awr-forest-night` (r18/r19).
Not bit-identical, and not yet explained: a flock of birds attacking in `awr-forest-night` (r19,
during the scripted attack, radial blur): 0.19-0.22/255 with no second-eye skips, the difference
confined to the birds (and 2.5/255 once in r18 during the attack with the flashlight burst); some
per-render state of that effect (swarm/particle step) is not rewound yet.

Exposure (`0x480340`) needs no rewind in this path: it adapts from the previous slot to the
current one in each eye with the same frame time, so both eyes compute the same exposure (the
frame path instead adapted twice per update).

### Second eye: work reused instead of redone (`skip_shadows`, `skip_probe`, default on)

- **Shadow pass** `0x50dba0` (static: called once per `renderWorld`, early from `0x5090b0` or late
  from `0x506b20`; shadow-filter globals, shadow manager begin RendererX86+0xf0 vtbl+0x18, each
  visible light's shadow render vtbl+0x40). For the second eye the hook returns without calling it:
  the light objects keep the first eye's shadow maps and shadow matrices (same update, same lights).
  Cascades are fitted to the first eye's frustum; the second eye's is 65 mm to the side.
- **Sky cube probe** `0x512220` (from the sky/atmosphere update `0x4a85d0`: the 6-face sky cube via
  the static PerspectiveView `0xdc44f0`, its filtering, the SH terms; `g_sAmbientSHTerms`,
  `g_sEnvironmentReflectionCube`): skipped for the second eye.

Draws recorded (live census, prologue-road): first eye ~4200 before the kick + ~5700 after; second
eye with both skips ~2960 + ~5720 (−1370 draws: the shadow pass and probe). The large second-batch
lists (2600-2800 and 2000-2200 draws) are view-dependent geometry and stay.

Verification (measured):
- Separation 0 with the skips (second eye reuses, first eye rendered them): prologue-road 0.054-0.118
  vs 0.052-0.111/255 without (r7); forest-night with the flashlight, interleaved off/on ×3 (r18):
  0.007-0.012 vs 0.004-0.009/255, ×5 during the bird attack (r19): same level for off and on except
  the flock (see above). Each skip alone 0.008/255 (r18).
- Disparity field at 32.5 mm with skips off/on (prologue-road r7): player 29.03 / 28.91 px (probe
  only 28.54, second off-capture 28.64), median of all patches +3.91 / +4.01 px, bins 20-60 / 10-20 /
  5-10 / 1-5 px 170/529/391/989 vs 178/526/419/929 (captures ~70 updates apart, idle motion).
  Forest-night (r18): player 29.95 off, 30.12 / 30.15 on.
- Shadows in the second eye: present and in place with the reuse (flashlight beam, the boat's and
  the debris's shadows; crop below). Frustum-edge artifacts: none seen at the left/right edges of
  the right eye; at 65 mm the cascade fit margin covers the offset.
- An A/B "render the second eye twice in one update, once with and once without the skips" lever
  was tried and dropped: the third render is not equivalent (per-frame stamps already taken), so
  its differences measured the lever, not the skips.

![second eye with and without reused shadows](img/stereo-eye2-shadows-skip.jpg)

*Right eye (rendered second), awr-forest-night r18, separation 32.5 mm: left rendered in full,
right with the first eye's shadow maps and sky probe. Captures ~100 updates apart (the player
turned slightly); shadows of the boat, debris and the flashlight beam are the same.*

### Battery on the view path (measured, instance awr-pass run r16)

`re/scripts/stereo_battery.sh` + `stereo_analyse.py`, prologue-road, 3840x1080 borderless, view
path with both reuses on, TAA and object velocity per eye (master defaults). (Run numbers in this
section are the awr-pass instance's.)

| | view path (r16) | section 2 (frame path, TAA off, awr r16) |
|---|---|---|
| sep 0: \|L−R\| mean, disparity median | **0.036 / 0.038 /255** (max 6), +0.001 / 0.000 px (p5..p95 ±0.04) | 0.81/255 (max 14), +0.009 px (−0.23..+1.29) |
| 16.25 / 32.5 / 65 mm: median of all patches | +1.79 / +3.94 / +7.63 px | +2.62 / +4.64 / +8.96 px (other camera pose) |
| per-patch ratio 32.5/16.25, 65/16.25, 65/32.5 (requested 2, 4, 2) | **1.999, 3.984, 1.996** | 2.00/1.89/1.95/1.84 by bin, 3.05-3.85 |
| field at 32.5 mm, 20-60 / 10-20 / 5-10 / 1-5 px | 190 / 525 / 415 / 915 patches | 423 / 364 / 435 / 769 |
| pan 0.5°/update, far field: sbs halves vs alternate | +3.29..+3.62 px vs **+10.2 / +11.1 px** | +1.73 / +1.76 vs +12.69 / +12.35 px |
| convergence 4 m: recovered Zc | 10.2 m (TAA on; see open issues) | 3.90 m |

The scaling is now essentially exact because both eyes of a pair are one frame and the separations
are taken minutes apart only between pairs. The player measured 29.07 px at 32.5 mm (r16, median of
the patches ≥ 20 px), as in section 2 (29.4 px).

![view path, both eyes in one frame](img/stereo-view-sbs.jpg)

*3840x1080 view-path frame (downscaled), r16, prologue-road, 32.5 mm, TAA per eye: both eyes from
one engine frame, the HUD hint drawn once and duplicated into both halves.*

### Cost (measured, `quiver lab gpu --for awr-pass -- re/scripts/stereo_rate.sh awr-pass`, r16)

GPU to itself (only awr-pass on it, 3840x1080 gamescope display, borderless, prologue-road, TAA and
object velocity on per eye), two rounds:

variant | renders/s | displayed frames/s | CPU ms per update (update / eye 1 / eye 2 / endFrame)
---|---|---|---
off (mono) | 142.5 / 143.3 | 142.5 / 143.3 | 5.72 / 5.36 / – / 0.27
frame (ModeSwitch::render ×2) | 146.2 / 145.7 | 73.2 / 72.7 | 12.3 / 5.44 / 6.07 / 0.60
view (renderFrame ×2) | 146.3 / 144.3 | 73.2 / 72.3 | 12.4 / 5.65 / 6.18 / 0.21
view + shadow and probe reuse | 163.4 / 159.2 | **81.8 / 79.5** | 11.0 / 5.02 / 5.42 / 0.20

Earlier, with the GPU shared (awr-taa rendering; r13): mono 69.4, frame 34.7, view 34.6, view +
skips 39.6-39.9 fps (+14 %); shadows alone 39.4, probe alone ~0 (within noise). In this scene the
game is CPU-bound on its single render thread when it has the GPU (≈5.5 ms of recording per eye),
so the view path's savings (one beginFrame/endFrame and Present, the HUD once) are small and the
reuse gains about 10 %; when the GPU is the limit, the reuse gains more. The rest of a render is
view-dependent.

### Open issues

- ~~Convergence with TAA on~~ (fixed, below): with the post-projection shift, parallel −
  converged at 4 m measured +6.2 px (frame path +5.1, view +6.5) instead of a uniform +18.86 px,
  i.e. the TAA resolve largely removes it. Film grain identical in both eyes biases the
  disparity tool towards 0 in flat fog, so measure convergence with grain off.
- The bird flock in forest-night differs between eyes (above).
- ~~Lab: 1920x1080 display, `output = full`, windowed: engine shrinks to 160x240~~ (fixed in
  window.cpp: the engine always stays at the game's resolution; docs/stereo-output.md).
- One crash in forest-night ~10 s in-world (r17, view path, no skips), not reproduced in r18/r19.
- **Shadow reuse (`skip_shadows`) is off by default** (lead's integration test, awr-wide,
  level 15, 3840x1080 borderless, all other defaults):
  - Skipping the whole shadow pass for the second eye made the game exit (clean, no exception,
    nothing in vkd3d.log) ~9 s in-world, as control returns after the opening cinematic and the
    flashlight comes on: 3 of 3 runs. Bisected live: frame path alive, view path without skips
    alive, probe reuse alone alive, shadow reuse alone exits.
  - Guard added: the second eye skips only if every light in its shadow list was in the first
    eye's (the frusta differ; an eye-2-only light was lit with shadow state nobody prepared).
    With it: no exit in 50 s runs and a walk through the yard; fallbacks 157-247 against
    3850-5437 reuses (live, `stereo status` `shadow_fallbacks`).
  - Even with the guard, the second eye loses the flashlight's lit spot on a near wall: same
    position, reuse on / off / on, right-eye crops show the spot only with reuse off (live
    captures, run r15). So the per-light shadow render also prepares lighting state the second
    eye needs (a cookie or per-render light constants; not identified). Until that is found the
    reuse stays opt-in; the probe reuse stays on.

### Convergence as image shift (full SBS)

In sbs with `output = full`, convergence is no longer put into the projection. Each eye's
finished image is shifted horizontally when it is copied into its half (`gpu_place_mid`, and
`gpu_compose` on the frame path): the left eye by `-a`, the right eye by `+b` pixels, where
`a + b = round(2 * e * m00 / Zc * W/2)` (e = separation per eye, W = eye width), the uncovered
columns black (a zero-initialised texture). This is the classic horizontal image translation:
it moves the zero-parallax plane to Zc exactly as the projection shift does for a parallel rig,
but after TAA, so the resolve cannot pull it back, and before the HUD, which stays at the
screen plane. The other modes (left/right/alternate, half output) keep the projection shift.

Measured (prologue-road, 32.5 mm, grain off, per-patch parallel − converged, one 3840 frame each):

| | applied | measured median | recovered Zc (4 m set) |
|---|---|---|---|
| TAA on, each eye rounded (9/9) | 18 px | +17.98 px (p5..p95 +11.4..+20.5, n 2253) | 4.20 m |
| TAA off, same | 18 px | +17.99 px (n 2356) | 4.19 m |
| TAA on, total rounded and split (9/10) | 19 px | **+19.04 px** (n 1277) | **3.96 m** |

The projection shift for comparison: +6.2 px with TAA on (above). Ideal K/Zc = 18.86 px.

---

## Reproducing

```sh
scripts/build
cp -r build/mod /tmp/modfull
sed -i 's/^output = half/output = full/; s/^start_level =$/start_level = continue/; s/^skip_videos = 0/skip_videos = 1/' \
    /tmp/modfull/torchbearer.ini                     # 3840 swapchain; boot straight to the checkpoint
quiver lab fixture install awr prologue-road        # game stopped
quiver lab launch awr --mod /tmp/modfull --wait frame
quiver lab wait awr in-world                         # ~11 s with the levels feature (docs/levels.md)
# (without [dev] start_level: re/scripts/awr_to_world.sh awr steps warning -> title -> Continue)
re/scripts/stereo_battery.sh awr                     # captures into the run folder
python3 re/scripts/stereo_analyse.py <run folder>
quiver lab gpu --for awr -- re/scripts/stereo_rate.sh awr 40 "off frame view view+skip"   # per variant
quiver lab call awr stereo census 2                  # frame-structure trace into torchbearer.log
quiver lab call awr stereo census 2 cl               # ... plus command lists touching the back buffer
re/scripts/stereo_taa_battery.sh awr                 # per-eye temporal state variants (section 5)
python3 re/scripts/stereo_taa_analyse.py <run folder>
VARIANTS="dlssshared:1:1:0 dlssreset:1:1:1" re/scripts/stereo_taa_battery.sh awr   # with game.dlss on
re/scripts/stereo_dlss_battery.sh awr                # per-eye DLSS vs reset vs TAA off (section 5, DLSS)
python3 re/scripts/stereo_taa_analyse.py <run folder> --variants ref,dq,dqr,dp,dpr [--eye-width 960]
```

Lab verbs: `stereo status | census N [cl] | shots N TAG [SKIP] | files | probe N | peek r|img|ADDR [+OFF|*]...`, `tweak list [SUBSTR] | tweak NAME
[VALUE]`, `cfg stereo.<key> <value>`. Ghidra: `re/scripts/Serve.java` (headless query server) and
`re/scripts/ghq` (client).
