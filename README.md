# SuperMario64DSRecomp

Very early, selection-only scaffold for a future Super Mario 64 DS static
recompilation target. It does not boot or execute the game yet.

This is a sibling game repository in the same shape used by the SNES and PSX
recomp projects:

```text
ndsrecomp/
  ndsrecomp/                 shared Nintendo DS framework repository
  supermario64dsrecomp/     this per-game repository
```

The local `ndsrecomp` entry inside this directory is an ignored junction to the
sibling framework. CMake builds the real ndsrecomp ARM core/recompiler and the
small `SuperMario64DSRecomp` selector together. There is intentionally no game
generated C or dispatch table yet.

## Current local ROM identity

The scaffold is pinned to a user-owned USA revision 0 dump:

| field | value |
|---|---|
| file | `Super Mario 64 DS.nds` (Git-ignored) |
| title | `S.MARIO64DS` |
| game code | `ASME` |
| size | 16 MiB |
| SHA-1 | `a5b8bd846725965c588b6fc591f1fe822f7bbdbd` |
| SHA-256 | `916056a27ffb07d1bae2ce0ca6f6464c1621c737dc96657a7b9eab4d3bccfc87` |

The ROM is local input only. Never commit it, extracted files, generated code,
screenshots, or other Nintendo material.

## SM64DS decomp reference

[`tangosdev/sm64ds-decomp`](https://github.com/tangosdev/sm64ds-decomp) is
pinned in `sm64ds-decomp.pin`. Its original work is MIT-licensed and provides
a valuable module registry, symbol names, relocations, types, and matching C.

Important: upstream currently targets the **European** retail build; this
scaffold's ROM is the **USA** `ASME` build. The decomp is therefore a
nomenclature, structure, and future region-mapping reference for now. Its EU
addresses and hashes must not be imported directly into the USA config.
The optional checkout lives under ignored `third_party/sm64ds-decomp/`.

The first integration seam is intentionally metadata-only. It exports every
upstream symbol with its module identity preserved (main/ITCM/DTCM/overlay),
because overlay address ranges overlap:

```sh
python tools/export_decomp_symbols.py
```

This writes ignored `generated/eu_decomp_symbols.tsv` and labels every row as
European. Nothing consumes those addresses as USA addresses.

## Local setup

From this directory on Windows, recreate the ignored sibling link and optional
decomp checkout with:

```powershell
New-Item -ItemType Junction -Path ndsrecomp -Target ..\ndsrecomp
git clone --depth 1 https://github.com/tangosdev/sm64ds-decomp.git third_party/sm64ds-decomp
git -C third_party/sm64ds-decomp checkout --detach 3b7450212bf0fae706a3be9bbd2d31d5c588b54f
```

Copy your own verified `ASME` revision 0 dump to
`Super Mario 64 DS.nds`. All three local inputs are ignored by Git.

## Build without booting

Prerequisites are CMake 3.20+, Ninja, and a C++20 compiler.

```sh
cmake -G Ninja -S . -B build
cmake --build build --target supermario64dsrecomp
```

The selector supports two metadata-only choices:

```sh
./build/SuperMario64DSRecomp --list
./build/SuperMario64DSRecomp --select firmware
./build/SuperMario64DSRecomp --select sm64ds --rom "Super Mario 64 DS.nds"
```

The SM64DS selection verifies the header, revision, size, and SHA-1, prints
`boot_status=not_implemented`, and exits without executing guest code.

## Next real integration steps

1. Establish a verified EU-to-USA module/address mapping, or add a USA branch
   to the decomp workflow.
2. Add a framework-owned NDS cartridge parser and per-game config schema.
3. Emit ARM9, ARM7, and overlay banks from the local ROM into ignored
   `generated/`.
4. Link those banks into a per-game runtime only after the DS cartridge boot
   path and overlay lifecycle exist in ndsrecomp.

No project-wide license has been declared for this scaffold yet. Upstream
references retain their respective terms.
