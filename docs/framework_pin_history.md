# Framework pin history

The `ndsrecomp` framework and the `third_party/recomp-ui` launcher UI are
pinned as Git submodules. `game.toml`'s `[framework] pin` records the same
framework commit so the packagers can validate it without Git.

| date | ndsrecomp | recomp-ui | notes |
|---|---|---|---|
| 2026-08-23 | `46b12e6c18dea47f87d2c1f98c3054149dcbca5d` | `3370e43253b19d1f4518cd1e0c96eb5deb45f989` | v0.0.2 release pin |
| 2026-08-26 | `12ee1b9fcdfd13eb66b844eddc7fc209c9ec3a43` | `76392fb215652a94f5b3b113806b9492731e2807` | SDL3-default frontend, frame interpolation, live-overlay tier-2, deadline-bounded per-instruction machinery |

## 2026-08-26 reconciliation to ndsrecomp `12ee1b9`

Framework landings absorbed by this bump:

- SDL3 is the default runner frontend (`NDS_SDL_BACKEND=SDL3|SDL2|NONE`).
- Frame interpolation (experimental, presentation-only).
- Live-overlay tier-2 player toolchain.
- Deadline-bounded per-instruction machinery (~19% emulation-time win,
  byte-identical semantics). The faithful path stays selectable in the same
  binary with `NDS_CYCLE_FAST_LIMIT=0`.

Repository changes required:

- `CMakeLists.txt` picks up `external/arm-recomp-core` include directories via
  `arm_recomp_core_include_dirs()` when the framework ships that tree, and
  feeds them to `sm64ds_recompiled_banks`.
- The launcher gate accepts `MINGW`/`MSYS` in addition to bare `WIN32`.
- The launcher gained an MPH-style SDL backend switch,
  `SM64DS_LAUNCHER_SDL_BACKEND=SDL3|SDL2` (default `SDL3`), forwarded to
  recomp-ui through `SNESRECOMP_SDL_BACKEND` instead of hard-forcing
  `RECOMP_UI_SDL3=OFF`.
- `tools/build-windows.ps1` and `tools/make_release.ps1` take
  `-SdlBackend SDL3|SDL2` (default `SDL3`), forward it to the runner
  (`NDS_SDL_BACKEND`) and the launcher (`SM64DS_LAUNCHER_SDL_BACKEND`), and
  stage `SDL3.dll` instead of `SDL2.dll`. SDL3 builds additionally stage
  `libiconv-2.dll`: MSYS2's SDL3 links it, and omitting it is what shipped
  MPH v0.6.0/v0.6.1 builds that failed to start on player machines while
  looking fine on a dev box with `mingw64\bin` on `PATH`.

Bank regeneration is a no-op at this pin: all 71 generated bank translation
units are byte-identical to the ones emitted at `46b12e6`. Any behaviour
change at this pin therefore comes from the runner, not from recompiled code.

### Open blocker: firmware cartridge boot hangs at this pin

Verified 2026-08-26 with a runner built from `12ee1b9` and the regenerated
(byte-identical) banks:

- `--boot direct` is healthy. The title screen ("TOUCH TO START") is reached,
  `vblank9` advances ~360 per 6 s (~60 fps), `irq9`/`irq7`/`timer_ovf`/FIFO
  traffic all advance, and `tier3_coverage` holds only a small handful of
  low-hit entries.
- The default firmware cartridge boot (`startup_mode = "automatic"`,
  `NDS_BOOTSTRAP_FIRMWARE=ON`) deadlocks. ARM9 spins in the high exception
  vectors at `r15 = 0xFFFF03E4` with `r0 = 0x04000180` (`IPCSYNC`); ARM7's
  dispatch counters freeze at exactly 204,124 dispatches; `vblank9`, `irq9`,
  `irq7` and `timer_ovf` never advance; both screens present pure white.
- Not the new machinery: `NDS_CYCLE_FAST_LIMIT=0` reproduces the hang
  identically (`cycle_fast_limit: false`, `fast_limit_publishes: 0`).
- Not the banks: byte-identical to the `46b12e6` emit, and a runner built
  before this pin boots the same ROM through firmware normally.

Until that is root-caused, firmware cartridge boot should not be assumed
working at this pin; `--boot direct` is the verified attract route.
