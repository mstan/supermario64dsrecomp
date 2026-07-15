#!/usr/bin/env python3
"""Export the pinned EU decomp's symbols as module-qualified local metadata.

The output is deliberately written under ignored generated/. Overlay address
ranges overlap, so address-only symbol tables are unsafe for this game.
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DECOMP = ROOT / "third_party" / "sm64ds-decomp"
DEFAULT_OUT = ROOT / "generated" / "eu_decomp_symbols.tsv"
SYMBOL_RE = re.compile(
    r"^(?P<name>\S+)\s+kind:(?P<kind>[^()\s]+)"
    r"(?:\((?P<attrs>[^)]*)\))?\s+addr:(?P<addr>0x[0-9a-fA-F]+)"
    r"(?:\s+.*)?$"
)


def symbol_sets(decomp: Path) -> list[tuple[str, Path]]:
    config = decomp / "config" / "arm9"
    sets = [
        ("main", config / "symbols.txt"),
        ("itcm", config / "itcm" / "symbols.txt"),
        ("dtcm", config / "dtcm" / "symbols.txt"),
    ]
    sets.extend(
        (path.parent.name, path)
        for path in sorted((config / "overlays").glob("ov*/symbols.txt"))
    )
    return sets


def parse_symbol(line: str) -> tuple[str, str, str, str, str] | None:
    match = SYMBOL_RE.match(line.strip())
    if not match:
        return None
    attrs_text = match.group("attrs") or "any"
    attrs = [part.strip() for part in attrs_text.split(",")]
    mode = attrs[0] if attrs else "any"
    size = ""
    for attr in attrs[1:]:
        if attr.startswith("size="):
            size = attr.removeprefix("size=")
    return (
        match.group("addr").lower(),
        match.group("kind"),
        mode,
        size.lower(),
        match.group("name"),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decomp", type=Path, default=DEFAULT_DECOMP)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    sets = symbol_sets(args.decomp)
    missing = [str(path) for _, path in sets if not path.is_file()]
    if missing:
        parser.error("missing decomp symbol files: " + ", ".join(missing))

    rows: list[tuple[str, str, str, str, str, str, str]] = []
    for module, path in sets:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parsed = parse_symbol(line)
            if parsed is None:
                raise ValueError(f"unrecognized symbol line {path}:{number}: {line}")
            address, kind, mode, size, name = parsed
            rows.append(("Europe", module, address, kind, mode, size, name))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow(("region", "module", "address", "kind", "mode", "size", "name"))
        writer.writerows(rows)

    print(f"exported {len(rows)} EU symbols from {len(sets)} modules to {args.out}")
    print("warning: these addresses are not valid for the USA ASME ROM without mapping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
