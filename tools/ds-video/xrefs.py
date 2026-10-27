#!/usr/bin/env python3
"""Find code sites that reference a string or constant in a prelinked kext.

The scaler and JPEG kexts have no symbol table, and Ghidra does not always
create functions for the C++ virtual methods, so `get_xrefs_to` can come back
empty for exactly the strings we care about. This script decodes the kext's
__TEXT_EXEC directly out of the kernelcache and resolves the two instruction
forms Apple's compiler uses for every literal reference:

    adrp xN, page            ; word & 0x9f000000 == 0x90000000
    add  xN, xN, #imm12      ; word & 0x7f800000 == 0x11000000

  tools/ds-video/xrefs.py 0xfffffff0054c9983                 # one target
  tools/ds-video/xrefs.py --strings AppleAJPEGHal            # every string
  tools/ds-video/xrefs.py --bundle AppleAJPEGHal --near 0xfffffff00554c9000

The reported "site" is the referencing instruction. It is *not* a function
start - ask Ghidra (`get_function_by_address`) or read backwards for the
prologue before naming the enclosing method.
"""
import argparse
import struct
import sys

sys.dont_write_bytecode = True          # keep the source tree clean
sys.path.insert(0, __import__("os").path.dirname(__file__))
import importlib.util
import os

_spec = importlib.util.spec_from_file_location(
    "kck", os.path.join(os.path.dirname(os.path.abspath(__file__)), "kc-kexts.py"))
kck = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kck)


def adrp_target(pc, w):
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    return ((pc & ~0xFFF) + (imm << 12)) & 0xFFFFFFFFFFFFFFFF, w & 0x1F


def add_imm(w):
    if w & 0x7F800000 != 0x11000000:
        return None
    shift = (w >> 22) & 1
    imm12 = (w >> 10) & 0xFFF
    return (w & 0x1F), ((w >> 5) & 0x1F), imm12 << (12 if shift else 0)


def scan(bundle, targets=None, range_hint=None, kc=kck.KC):
    table = kck.kext_table(kc)
    bid, vm, hdr_foff, segs = kck.find_kext(table, bundle)
    data = kck.load_kc(kc)
    exec_seg = [s for s in segs if s[0] == "__TEXT_EXEC"][0]
    name, vma, vmsz, fo, fs, prot = exec_seg
    blob = data[hdr_foff + fo:hdr_foff + fo + fs]
    words = struct.unpack_from(f"<{len(blob)//4}I", blob)
    tset = set(targets) if targets else None
    hits = []
    for i in range(len(words) - 1):
        pc = vma + i * 4
        if words[i] & 0x9F000000 == 0x90000000:
            page, rd = adrp_target(pc, words[i])
            nxt = words[i + 1]
            # add xN, xN, #imm  (must chain on the same register)
            a = add_imm(nxt)
            cand, chained = None, False
            if a and a[0] == rd and a[1] == rd:
                cand, chained = page + a[2], True
            elif page:                       # bare page reference (e.g. ldr [xN])
                cand = page
            if cand is None:
                continue
            if tset is not None:
                if not chained:
                    continue                 # exact targets need the add, not the page
                if cand not in tset:
                    continue
            elif range_hint and not (range_hint[0] <= cand < range_hint[1]):
                continue
            hits.append((pc, cand))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--strings", default="", help="bundle substring: resolve every string first")
    ap.add_argument("--bundle", default="", help="bundle for --strings")
    ap.add_argument("--near", default="")
    ap.add_argument("--kc", default=kck.KC)
    args = ap.parse_args()

    if args.strings:
        bundle = args.bundle or args.strings
        # use the kext's own string table
        table = kck.kext_table(args.kc)
        bid, vm, hdr_foff, segs = kck.find_kext(table, bundle)
        data = kck.load_kc(args.kc)
        text = [s for s in segs if s[0] == "__TEXT"][0]
        blob = data[hdr_foff + text[3]:hdr_foff + text[3] + text[4]]
        targets, names = {}, {}
        i, n = 0, len(blob)
        printable = set(range(0x20, 0x7f))
        while i < n:
            if blob[i] in printable:
                j = i
                while j < n and blob[j] in printable:
                    j += 1
                if j < n and j - i >= 4:
                    s = blob[i:j].decode()
                    if args.strings.lower() in s.lower():
                        targets[text[1] + i] = s
                i = j
            i += 1
        for pc, cand in scan(bid, list(targets), kc=args.kc):
            label = ""
            for t, s in targets.items():
                if cand <= t < cand + 0x1000:
                    label = s
                    break
            print(f"site={pc:#x}  {label}")
        return

    tset = [int(t, 0) for t in args.targets]
    hint = None
    if args.near:
        a = int(args.near, 0)
        hint = (a, a + 0x1000)
    for pc, cand in scan("AppleJPEGDriver", tset or None, hint, kc=args.kc):
        print(f"site={pc:#x} target={cand:#x}")


if __name__ == "__main__":
    main()
