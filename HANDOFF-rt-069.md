# Hand-back: RT foliage / RTAO v2 / flashlight (for Claude session `torchbearer`)

Written 2026-11-20 ~14:07 MST by Muse (muse-wonder-full-1.0), picking up subagent
`a05b775f5244f01c0`, which the weekly limit stopped mid-edit at 19:53Z. Delete this file once read.

Release update, 2026-11-20 ~14:42 MST: the user authorized Muse to commit, push, and publish.
The branch was merged to `master` in `0679d67`, the merged tree was rebuilt and lab-smoked
(r79), and Quiver Torchbearer **0.6.9 (`vx8mzgd`, mod revision 27) was published and is
current**. A Git push could not then be completed because this repository had **no remotes
configured** (`git remote -v` was empty); no remote was guessed or added at that point.

GitHub update, 2026-11-20 MST: at the user's request, a public MIT-licensed repository was then
created at https://github.com/AndrewGaspar/torchbearer, with a screenshot-led README maintained
under `docs/screenshots/`, and `master` was pushed there (initial public tip `f55133a`, remote
`origin`).

## Where things are

- The work was developed in `.claude/worktrees/agent-a05b775f5244f01c0` on branch
  `worktree-agent-a05b775f5244f01c0`, then merged to `master` in `0679d67`. The merge and later
  Muse release commits carry `Co-Authored-By: Muse Wonder Full 1.0`; the earlier branch
  commits were not rewritten.
- Commits on this branch (oldest first):
  - `dca0d43` — your subagent's recovered WIP, committed as found (ray-query trace rewrite,
    `rays.hlsl`/`alpha.hlsli`, spotlight bindings in shader only). It built and traced, but the
    scene/census producer half did not exist and `EyeInputs.w2c` was never filled.
  - `3e4297f` — census alpha capture (descriptor tables/heaps, RS range parse, threshold + UV-form
    reflection, DrawRec alpha fields).
  - `23c2238` — scene producer (records, `cs_gather` jobs, draw-time SRV→resource capture,
    non-opaque BLAS, instance motion) + the `w2c` fill + validator `noalpha` baseline.
  - `7d14c78` — spotlight bindings completed (alpha SRV, game-heap table t0 space10, sampler,
    `tb_q`, `dev.rt_spot_*` tuning).
  - `d8f48ba` — `[graphics] flashlight_shadows = game|rt` (restart), AO-chain gating for
    flashlight-only, HBAO branch writes split.
  - `68460d9` — docs sections 13–14 + version 0.6.9.
  - `ab3dad7`, `41ca286`, `1ca8811` — stereo/ferry measurement notes in the docs.
  - `0679d67` — annotated merge to `master` for the published 0.6.9 build.
- Lab: `awr-rt` is **down**, fixture `awr-forest-night` re-installed. Its manifest no longer has
  `VKD3D_CONFIG=single_queue` / `VKD3D_DEBUG=warn` (removed per your brief; everything below ran
  without them). Staged test mods used: `/tmp/tb-wip-rtao` (ao=rtao + dev rt_spot=2),
  `/tmp/tb-flash` (flashlight only), `/tmp/tb-def` (defaults), `/tmp/tb-sbs*` (stereo),
  `/tmp/tb-final` (shipped defaults + both RT options). DXC 1.9 is in this worktree's
  `build/tools/dxc` (your subagent downloaded it; `scripts/build_shaders` works).

## Proven (lab, this branch)

- Inert defaults: no capture hooks (r66). Flashlight-only: scene on, AO chain off, swap+draws
  bound (r67). RTAO: copies every frame (r69/r75). Combined final config smoke r75.
- Foliage validation (r64, interleaved A/B ×3, `validate` vs `validate noalpha`): foliage-heavy
  view exact 1,122,194 → 1,235,451, too-near 264,363 → 118,870; gameplay view exact +20,863,
  too-near halved. Full bins in docs/raytracing.md section 13.
- Flashlight: swap once, thousands of bound draws, renders correctly RT vs fallback (`rt_spot_on`
  A/B, r65); runs in sbs/hsbs with `skip_shadows = 1` (r73). One earlier stereo launch (r70)
  exited once and did not reproduce in r71–r73 — treated as a flake, worth a soak if you touch it.
- Costs (uncontended timestamps): AS+gather ~1.25 ms, AO rays +0.85 ms, denoise ends 2.7 ms.

## Decisions that differ from the subagent's apparent plan (review these)

1. **Root signatures: kept the global append**, extended to 4 params + sampler — not the
   per-PSO copy the `spotlight.hlsl` comment described (comment updated to match). Reason:
   identity-preserving and proven in the prototype; a per-PSO copy risks the game re-setting the
   original signature after `SetPipelineState`. Harden later if you disagree.
2. **Alpha textures are resolved to resources at record time** (CreateSRV/CopyDescriptors map),
   and the mod creates its own SRVs per update. Copying CPU descriptors at the kick names the
   wrong texture: the game slides a bindless window and rewrites descriptors per draw (r60).
3. **HBAO render is not skipped** for RTAO. Tried; every AO copy missed (142/142, r68) because
   the apply's source (`obj+0x18`) is the render's pick. Branch writes are split if you find
   another delivery path.
4. Alpha threshold/UV classification is pattern-based reflection (0.2/0.5/0.65; modes 1/2/3 in
   section 13). One `$Globals`-threshold shader family (e.g. PS `3d102abaf873b572`) stays opaque.

## Not done / not verified

- Stereo second-eye lost spot: aimed A/B done (r76 forest, r77 apartment; pad LT, sbs/hsbs +
  `skip_shadows = 1`): eyes match in both modes, no second-eye darkness — but forest captures
  are floodlight/encounter-dominated and Alan has no flashlight in the apartment dream, so
  treat those captures as consistent-with, not isolated proof. Codex later isolated the spot in
  both eyes on level 03's dark rock face (r80, confirmed aiming and shadow reuse). The game
  shader also passed there. A direct RT comparison at the original level 15 full-SBS failure
  location remains open. See [the measurements](docs/raytracing.md#aimed-flashlight-in-stereo-on-a-dark-rock-face).
  Note `awr-rt` now has `pads=1` in its manifest.
- Ferry temporal improvement: measured (r78 bench A/B): temporal noise 18.68 ± 0.40 with
  instance motion vs 22.17 ± 1.42 without. Done; no further ferry work queued.
- Skinned close-up banding: receiver-from-hit is on by default (`dev.rtao_receiver`); no close-up
  capture was taken.
- Flashlight cost inside the deferred-light pass not isolated (no marker); atlas still renders
  (other `g_sShadowMapTex` consumers — keep).
- Sun/moon: unchanged prototype (now benefits from alpha path in principle; not re-validated).
- LOD-aware alpha sampling (SampleLevel 0 today) is the known distant-foliage error source.
- RTGI / realistic night lighting: untouched, per the roadmap note in your memory.

## Resume

SendMessage the subagent (`a05b775f5244f01c0`) rather than starting a new one, per the limit
notice — but its context predates all of the above, so point it at this file and the commit list
first. The branch is already merged, 0.6.9 is published/current in Quiver, and `master` is
public at https://github.com/AndrewGaspar/torchbearer. The quiet armed dark-scene stereo-spot
test is recorded in r80. Sun/moon placement remains open. Keep the README screenshots
current when pictured features change (`docs/screenshots/README.md`).
