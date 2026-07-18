# SuperMario64DSRecomp

Very early pre-alpha scaffold for a future Super Mario 64 DS static
recompilation target. It recompiles and host-compiles the European game's
initial ARM9 and ARM7 code banks and now boots through the authentic
BIOS/firmware cartridge path into the title flow. It does **not support
gameplay yet**.

The shared Nintendo DS framework is pinned as the `ndsrecomp` Git submodule.
The ROM, extracted images, generated recompilation output, and optional
third-party checkout are all local ignored inputs.

## Verified local target

The scaffold is pinned to the European revision-0 retail build used by the
online decomp:

| field | value |
|---|---|
| file | `Super Mario 64 DS.nds` (Git-ignored) |
| title | `S.MARIO64DS` |
| game code | `ASMP` |
| size | 16 MiB |
| SHA-1 | `1367529f2cb23e76ef295cb1727333ae8f0a6cd7` |
| SHA-256 | `ce5829aaa79f06c67140ed1f45cbca738b0a9d0180538c8c1498472d859db032` |

Never commit the ROM, archive artwork/NFO files, extracted binaries, generated
code, screenshots, or other Nintendo material.

## Decomp-assisted recompilation

[`tangosdev/sm64ds-decomp`](https://github.com/tangosdev/sm64ds-decomp) is
pinned in `sm64ds-decomp.pin`. Its original MIT-licensed work supplies exact
European ARM9 function names, modes, sizes, and module boundaries. The scaffold
uses that metadata; it does not compile the decomp's matched C or copy it here.

`tools/prepare_sm64ds.py` verifies the local ROM, decompresses its ARM9 image,
and generates ignored ndsrecomp configs. The current build then:

- recompiles all 3,090 decomp-defined functions in the ARM9 main module;
- emits that bank with live-byte validation, because the game decompresses its
  ARM9 image in place at startup;
- discovers and recompiles the initial 14-function ARM7 boot closure;
- compiles the generated C banks into `libsm64ds_recompiled_banks.a`.

The ROM-derived outputs live under ignored `generated/` and are never suitable
for source control or release artifacts.

## Local setup and build

Initialize the framework submodule and pinned decomp checkout, then install
`ndspy` in the checkout's local virtual environment:

```powershell
git submodule update --init --recursive
git clone https://github.com/tangosdev/sm64ds-decomp.git third_party/sm64ds-decomp
git -C third_party/sm64ds-decomp checkout --detach 3b7450212bf0fae706a3be9bbd2d31d5c588b54f
py -3.12 -m venv third_party/sm64ds-decomp/.venv
third_party/sm64ds-decomp/.venv/Scripts/python.exe -m pip install ndspy
```

Place your own verified European dump at `Super Mario 64 DS.nds`, then build:

```powershell
cmake -G Ninja -S . -B build
cmake --build build --target supermario64dsrecomp
```

The build graph verifies and prepares the ROM, builds ndsrecomp, emits both
game banks, compiles them, and builds the selector. To inspect the current
boundary:

```powershell
./build/SuperMario64DSRecomp --select sm64ds --rom "Super Mario 64 DS.nds"
```

## Current boot boundary

The fake direct-boot shortcut remains deliberately unused. ndsrecomp now models
the physical-card command path used by the firmware, including chip identity,
ROM streaming, DMA, KEY1 commands, and normalization of already-decrypted ROM
secure areas before the BIOS decrypts them.

The tested local target passes the firmware menu, launches both game CPUs,
renders the Nintendo splash, accepts the game's `TOUCH TO START` input, and
reaches the main title flow without a terminal dispatch miss or CPU stall. At
the retained VBlank-1100 smoke boundary, native and the pinned melonDS oracle
match the core VBlank, IPC FIFO, SPI, and IRQ event counts.

The upper title screen is recognizable and close to the oracle. The lower
screen is not correct after the title transition: Engine B/3D composition is
incomplete, so the Mario-face/menu screen is currently blank or white. This is
a boot milestone, not a gameplay milestone. Next work includes the lower-screen
2D/3D path, the full 3D engine, NitroFS/overlay coverage, and broader ARM7/game
service validation.

No project-wide license has been declared for this scaffold yet. Upstream
references retain their respective terms.
