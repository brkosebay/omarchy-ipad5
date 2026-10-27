#!/usr/bin/env python3
"""Recover register programs from stripped Apple kext code.

The j71t kernelcache has no symbols for the scaler/JPEK kexts, but the HAL
methods are mostly straight-line code that does

    mov  w8, #0x1f4        ; value
    str  w8, [x19, #0x40]  ; offset inside the mapped block

so an ordered list of (offset, value) pairs can be lifted straight out of the
disassembly. This script disassembles a range with llvm-objdump, tracks small
integer immediates through one register, and prints every access to memory
with a slot for the recovered constant.

  tools/ds-video/regprog.py --start 0xfffffff005fe2834 --end 0xfffffff005fe3400
  tools/ds-video/regprog.py --start ... --end ... --only-str
  tools/ds-video/regprog.py --start ... --end ... --md

It is a reading aid, not a decompiler: values that arrive via `ldr` from
another structure are printed as `<reg>`, and helper calls are shown so the
shape of the sequence stays visible. Always confirm a pair against the
surrounding code before quoting it as a register offset.
"""
import argparse
import re
import subprocess
import sys

OBJDUMP = ("/Applications/Xcode.app/Contents/Developer/Toolchains/"
           "XcodeDefault.xctoolchain/usr/bin/llvm-objdump")
KC = "build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12"

LINE = re.compile(r"^\s*([0-9a-f]{16}):\s+(\S+)\s*(.*)$")
IMM = re.compile(r"#(-?0x[0-9a-f]+|-?\d+)")
BRACKET = re.compile(r"\[(x\d+)(?:,\s*#(-?0x[0-9a-f]+|-?\d+))?\]")


def dis(start, end, kc=KC):
    out = subprocess.run([OBJDUMP, "-D", "--arch=arm64", "--no-show-raw-insn",
                          f"--start-address={start:#x}", f"--stop-address={end:#x}", kc],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            yield int(m.group(1), 16), m.group(2), m.group(3).split(";")[0].strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    ap.add_argument("--kc", default=KC)
    ap.add_argument("--only-str", action="store_true")
    ap.add_argument("--md", action="store_true", help="markdown table output")
    args = ap.parse_args()
    start, end = int(args.start, 0), int(args.end, 0)

    imm = {}          # reg -> known constant
    rows = []
    for addr, op, rest in dis(start, end, args.kc):
        if op in ("mov", "movz", "movn", "orr") :
            parts = [p.strip() for p in rest.split(",")]
            if len(parts) == 2 and parts[1].startswith("#"):
                v = IMM.search(parts[1])
                if v:
                    imm[parts[0]] = int(v.group(1), 0) & 0xffffffff
                    if op == "movn":
                        imm[parts[0]] = (~imm[parts[0]]) & 0xffffffff
        elif op == "movk":
            parts = [p.strip() for p in rest.split(",")]
            m = re.match(r"#(-?0x[0-9a-f]+|-?\d+)(?:,\s*lsl\s*#(\d+))?", parts[1]) if len(parts) > 1 else None
            if m and parts[0] in imm:
                shift = int(m.group(2) or 0)
                mask = ~(0xffff << shift) & 0xffffffff
                imm[parts[0]] = (imm[parts[0]] & mask) | ((int(m.group(1), 0) & 0xffff) << shift)
        elif op in ("str", "strb", "strh", "ldr", "ldrb", "ldrh"):
            m = BRACKET.search(rest)
            if not m:
                continue
            base, off = m.group(1), m.group(2)
            off = int(off, 0) if off else 0
            src = rest.split(",")[0].strip().split()[0]
            val = ""
            if op.startswith("str"):
                if src in imm:
                    val = f"0x{imm[src]:x}"
                else:
                    val = f"<{src}>"
            else:
                imm.pop(src, None)
            rows.append((addr, op, base, off, val, src))
            continue
        if op == "bl":
            tgt = rest.strip()
            rows.append((addr, "bl", "", 0, tgt, ""))
            imm.clear()

    if args.md:
        print("| site | access | base | offset | value |")
        print("| --- | --- | --- | --- | --- |")
    for addr, op, base, off, val, src in rows:
        if args.only_str and not op.startswith("str"):
            continue
        if args.md:
            acc = f"{op} {src}" if src else "bl"
            print(f"| `{addr:#x}` | `{acc}` | `{base}` | `{off:#x}` | `{val}` |")
        else:
            print(f"{addr:#x}  {op:5s} {src or val:8s} [{base}, {off:#x}]  {val}")


if __name__ == "__main__":
    main()
