# Torchbearer

QoL and stereoscopic-3D mod for **Alan Wake Remastered** (Epic, `epic:7315f614b0504081bcdceef138075c9c`,
`Game_f_x64_EOS.exe`, Remedy's Northlight engine, **Direct3D 12** under vkd3d-proton), packaged as a
Quiver mod (`dist/quiver-mod.toml`) with a Quiver options surface (`dist/quiver-options.toml`,
surface `torchbearer`).

## Layout

- `src/proxy.cpp` — `dxgi.dll` proxy (Proton loads native dxgi from the game folder first); forwards
  to the system dxgi (DXVK's) and hooks factories → swapchains.
- `src/core/` — log (`torchbearer.log` in `%QUIVER_LAB_RUN_DIR%` in the lab, else next to the exe),
  config (`torchbearer.ini`, ids `section.key`, polled every 250 ms, also fed by the Quiver SDK;
  features subscribe with `cfg_listen`), hooks (MinHook `hook_fn`, `hook_vtable`), lab channel
  (`lab_state`, `lab_verb` → `quiver lab call awr <verb> …`), Quiver SDK glue, Present/Resize
  listeners (`present_on`, `resize_on`), `game_image()` for RVAs.
- `src/features/` — one file (or folder) per feature; register listeners/verbs in
  `features_register()`, install hooks in `features_start()` (`src/features/features.cpp`).
- `dist/` — `torchbearer.ini` (shipped defaults), `quiver-mod.toml`, `quiver-options.toml`.
- `re/` — reverse-engineering notes (`re/*.md`) and scripts (`re/scripts/`). `re/ghidra/` holds the
  analysed Ghidra project (gitignored, image base 0x140000000). **Never open `re/ghidra` directly
  from more than one process**: copy it (`cp -r --reflink=auto re/ghidra re/ghidra-$NAME`) and run
  `analyzeHeadless re/ghidra-$NAME awr -process Game_f_x64_EOS.exe -noanalysis -readOnly
  -scriptPath re/scripts -postScript X.py` against your copy. Ghidra lives in `/opt/ghidra`.
- `docs/` — design notes and findings, one topic per file.

## Build and run

```sh
scripts/build                                  # mingw-w64 → build/mod/{dxgi.dll,torchbearer.ini,…}
quiver lab launch awr --mod build/mod --wait frame
quiver lab logs awr                            # run folder; torchbearer.log is there
quiver lab set awr install:torchbearer.ini:[section]key=value   # live config change
quiver lab call awr cfg section.key [value]    # read / set a config value in memory
```

Always pass `--source agent:<name>` (or `QUIVER_SOURCE`) to quiver commands. Follow the lab rules
in `quiver lab brief awr` (no pkill/sleep sequences, poll real signals, `--settle` before
screenshots, never touch the real prefix/install, never route around the EOS proxy).

All lab instances are **1920x1080@60** unless a test needs otherwise, so screenshots and timings
compare across instances. The GPU (RTX 5070 Laptop, 8 GB) is shared with other sessions: check
`quiver lab host` before starting another in-world instance, and `quiver lab down` instances
you're done with.

## Conventions

- Addresses in notes and code are **RVAs** (or VAs at base 0x140000000, written `0x14xxxxxxx`);
  code resolves them through `game_image().at(rva)` and verifies a byte signature first, logging
  and disabling the feature on a mismatch (the game may be patched).
- Every claim in `docs/`/`re/` notes says how it was established (static, live, measured).
- Features must be inert when their config is off; a hook with the feature off is a pass-through.
