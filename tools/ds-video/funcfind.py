#!/usr/bin/env python3
"""Find the enclosing function of an address in a stripped prelinked kext.

`anno.py` only labels functions that some `bl` reaches directly, and IOKit
virtual methods are reached through vtables, so they have no label. This walks
back to the previous control-flow boundary (`ret`, unconditional `b`, `br`) and
reports the prologue, then lists every `bl`/`b` in the whole listing that
targets any address inside the recovered body.

Usage:
  tools/ds-video/funcfind.py build/video/kexts/AppleD5500.anno.txt 0xfffffff006292414
  tools/ds-video/funcfind.py ... 0x... --callers   # only the call sites
"""
import argparse
import re
import sys
from pathlib import Path

INSN = re.compile(r"^([0-9a-f]{16}):\s+(\S+)(?:\s+(.*))?$")
BOUNDARY = {"ret", "br", "b"}   # `b` is unconditional only when it has no condition


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("listing", type=Path)
    ap.add_argument("addr")
    ap.add_argument("--callers", action="store_true")
    args = ap.parse_args()

    target = int(args.addr, 16)
    rows = []
    for ln in args.listing.read_text().splitlines():
        m = INSN.match(ln)
        if m:
            rows.append((int(m.group(1), 16), m.group(2), (m.group(3) or ""), ln))

    # locate the requested instruction
    idx = next((i for i, r in enumerate(rows) if r[0] >= target), None)
    if idx is None:
        sys.exit("address not in listing")

    # walk back to the previous boundary instruction
    start = None
    for i in range(idx, 0, -1):
        mnem = rows[i][1]
        ops = rows[i][2]
        if mnem in BOUNDARY or mnem == "br" or (mnem == "b" and not ops.startswith(".")):
            # a `b` with a target is unconditional; conditional branches are b.<cond>
            if mnem == "b" and re.match(r"0x[0-9a-f]+", ops or ""):
                start = rows[i + 1][0] if i + 1 < len(rows) else None
                break
            if mnem == "ret":
                start = rows[i + 1][0] if i + 1 < len(rows) else None
                break
    if start is None:
        start = rows[0][0]

    end = None
    for i, r in enumerate(rows):
        if r[0] >= start and i > 0 and rows[i - 1][0] >= start and r[1] in ("ret", "br"):
            end = r[0]
            break
    end = end or rows[-1][0]

    body = [r for r in rows if start <= r[0] <= end]
    print(f"# enclosing function {start:#x} .. {end:#x} ({len(body)} insns)")
    for r in body[:8]:
        print(r[3])
    print("...")

    callers = [(a, ln) for a, mnem, ops, ln in rows
               if mnem in ("bl", "b") and (mm := re.search(r"(0x[0-9a-f]+)", ops)) and start <= int(mm.group(1), 16) <= end]
    print(f"# {len(callers)} direct call sites into this function")
    for a, ln in callers:
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
