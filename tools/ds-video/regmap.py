#!/usr/bin/env python3
"""Recover the MMIO register map a prelinked kext actually touches.

AppleD5500 (and most Imagination-derived drivers) reach registers through two
tiny helpers:  read = *(u32 *)(gRegBase + offset), write = *(u32 *)(gRegBase +
offset) = value. That makes the whole map recoverable from the instruction
stream: every `mov w1, #imm` feeding such a helper is one register offset.

Input is the annotated listing from anno.py (generated on demand).

Usage:
  tools/ds-video/regmap.py build/video/kexts/AppleD5500.macho
  tools/ds-video/regmap.py build/video/kexts/AppleD5500.anno.txt --read-helper 0xfffffff0062902b0 \
                                                            --write-helper 0xfffffff0062902e0
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOV_W1 = re.compile(r"mov\tw1, #(0x[0-9a-f]+)")
CALL = re.compile(r"bl\t(0x[0-9a-f]+)")
ADDR = re.compile(r"^([0-9a-f]{16}):")

# defaults for AppleD5500.macho (CPVDECCore register accessors)
READ_HELPER = 0xFFFFFFF0062902B0
WRITE_HELPER = 0xFFFFFFF0062902E0


def ensure_listing(macho: Path) -> Path:
    listing = macho.with_suffix(".anno.txt")
    if not listing.exists():
        print(f"# generating {listing} (anno.py)", file=sys.stderr)
        subprocess.run([sys.executable, str(ROOT / "tools/ds-video/anno.py"), str(macho)],
                       stdout=open(listing, "w"), check=True)
    return listing


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("--read-helper", type=lambda s: int(s, 16), default=READ_HELPER)
    ap.add_argument("--write-helper", type=lambda s: int(s, 16), default=WRITE_HELPER)
    ap.add_argument("--merge", action="store_true", help="print one row per offset")
    args = ap.parse_args()

    listing = args.input if args.input.suffix == ".txt" else ensure_listing(args.input)
    lines = listing.read_text().splitlines()

    reads = collections.defaultdict(list)
    writes = collections.defaultdict(list)
    for i, ln in enumerate(lines):
        m = CALL.search(ln)
        if not m:
            continue
        target = int(m.group(1), 16)
        if target not in (args.read_helper, args.write_helper):
            continue
        for j in range(i - 1, max(-1, i - 8), -1):
            mo = MOV_W1.search(lines[j])
            if mo:
                (reads if target == args.read_helper else writes)[int(mo.group(1), 16)].append(
                    ADDR.match(lines[i]).group(1))
                break

    def dump(title, table):
        print(f"# {title}: {len(table)} distinct offsets, "
              f"{sum(len(v) for v in table.values())} accesses")
        for off in sorted(table):
            sites = table[off]
            print(f"{off:#07x}  {len(sites):3d}  {', '.join(sites[:4])}"
                  f"{' ...' if len(sites) > 4 else ''}")

    if args.merge:
        allofs = sorted(set(reads) | set(writes))
        print(f"{'offset':>8}  {'r':>3} {'w':>3}   first read site")
        for off in allofs:
            print(f"{off:#8x}  {len(reads.get(off, [])):3d} {len(writes.get(off, [])):3d}   "
                  f"{(reads.get(off) or writes.get(off))[0]}")
    else:
        dump("reads", reads)
        print()
        dump("writes", writes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
