#!/usr/bin/env python3
"""Per-kext view of the prelinked iOS kernelcache (no ipsw, no Ghidra).

The j71t 20H350 kernelcache is a plain MH_EXECUTE whose kext Mach-O headers
live inside __PRELINK_TEXT and whose kext code lives in __PLK_TEXT_EXEC, so
`ipsw kernel extract` cannot split it. Every kext header is still a full
load-command set carrying that kext's own segment vmaddrs, which is all we
need to map an address back to a bundle, disassemble it in place, and read
its C strings.

  tools/ds-video/kc-kexts.py list [--filter SUBSTR]
  tools/ds-video/kc-kexts.py which ADDRESS [ADDRESS ...]
  tools/ds-video/kc-kexts.py segs BUNDLE_ID
  tools/ds-video/kc-kexts.py dis ADDRESS [--count N]      # N instructions
  tools/ds-video/kc-kexts.py str ADDRESS
  tools/ds-video/kc-kexts.py strings BUNDLE [--filter S] [--min-len N]
  tools/ds-video/kc-kexts.py dump ADDRESS --length N [--out FILE]

Addresses are unslid linked virtual addresses, i.e. the same numbers Ghidra,
`IOLog` backtraces and this repo's docs use.
"""
import argparse
import json
import os
import re
import struct
import subprocess

KC = "build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12"
PRELINK_JSON = "build/periph/kexts/prelink.json"
OBJDUMP = os.environ.get(
    "LLVM_OBJDUMP",
    "/Applications/Xcode.app/Contents/Developer/Toolchains/"
    "XcodeDefault.xctoolchain/usr/bin/llvm-objdump")

LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xFEEDFACF


def segments(data, off):
    magic, cputype, cpusub, ft, ncmds, sizeofcmds, flags, res = struct.unpack_from(
        "<IiiIIIII", data, off)
    if magic != MH_MAGIC_64:
        raise SystemExit(f"no MH_MAGIC_64 at {off:#x} (got {magic:#x})")
    out = []
    p = off + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, p)
        if cmd == LC_SEGMENT_64:
            name = data[p + 8:p + 24].split(b"\0")[0].decode(errors="replace")
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sgflags = \
                struct.unpack_from("<QQQQiiII", data, p + 24)
            out.append((name, vmaddr, vmsize, fileoff, filesize, initprot))
        p += cmdsize
    return out, ft


def load_kc(path):
    with open(path, "rb") as f:
        return f.read()


def kext_table(kc_path=KC, prelink_path=PRELINK_JSON):
    """[(bundle_id, header_vmaddr, header_fileoff, segments)] for every kext."""
    data = load_kc(kc_path)
    top, _ = segments(data, 0)
    pre = next(s for s in top if s[0] == "__PRELINK_TEXT")
    with open(prelink_path) as f:
        prelink = json.load(f)
    out = []
    for e in prelink:
        vm = e.get("_PrelinkExecutableLoadAddr") or 0
        size = e.get("_PrelinkExecutableSize") or 0
        if not vm or not size or not (pre[1] <= vm < pre[1] + pre[2]):
            continue                      # kpi pseudoextensions have no image
        foff = pre[3] + (vm - pre[1])
        try:
            segs, _ = segments(data, foff)
        except SystemExit:
            continue
        out.append((e.get("CFBundleIdentifier", "?"), vm, foff, segs))
    return out


def find_kext(table, needle):
    exact = [k for k in table if k[0] == needle]
    if exact:
        return exact[0]
    part = [k for k in table if needle.lower() in k[0].lower()]
    if len(part) == 1:
        return part[0]
    if not part:
        raise SystemExit(f"no kext matching {needle!r}")
    raise SystemExit("ambiguous: " + ", ".join(k[0] for k in part))


_TOP = {}


def top_segments(kc_path):
    """The kernelcache's own segment table: the only reliable vm -> file map.

    A kext header's segment fileoffs are *not* usable directly: the prelinker
    packs kext segments into __PRELINK_TEXT / __PLK_TEXT_EXEC / __PLK_DATA* and
    the padding differs from the vm layout (measured: 0x5000 for the JPEG
    __DATA_CONST). Mapping through the top-level table always agrees with
    Ghidra's read_memory.
    """
    if kc_path not in _TOP:
        data = load_kc(kc_path)
        _TOP[kc_path] = segments(data, 0)[0]
    return _TOP[kc_path]


def owner(table, addr, kc_path=KC):
    """(bundle, segment, seg_vmaddr, seg_vmsize, absolute_kernelcache_fileoff)."""
    abs_off = None
    for name, vma, vmsz, fo, fs, prot in top_segments(kc_path):
        if fs and vma <= addr < vma + vmsz:
            abs_off = fo + (addr - vma)
            break
    if abs_off is None:
        return None
    for bid, vm, hdr_foff, segs in table:
        for name, vma, vmsz, fo, fs, prot in segs:
            if vma <= addr < vma + vmsz:
                return (bid, name, vma, vmsz, abs_off)
    return None


def cmd_list(table, args):
    for bid, vm, foff, segs in table:
        if args.filter and args.filter.lower() not in bid.lower():
            continue
        code = [s for s in segs if s[5] & 4]
        span = ""
        if code:
            lo = min(s[1] for s in code)
            hi = max(s[1] + s[2] for s in code)
            span = f" code={lo:#x}..{hi:#x}"
        print(f"{bid:62s} hdr={vm:#x} size=0x{segs[0][2]:x}{span}")


def cmd_which(table, args):
    for a in args.addresses:
        addr = int(a, 0)
        hit = owner(table, addr, args.kc)
        if not hit:
            print(f"{addr:#x}  (not in any kext segment)")
            continue
        bid, name, vma, vmsz, foff = hit
        print(f"{addr:#x}  {bid}  {name}  seg={vma:#x}+{vmsz:#x} fileoff={foff:#x}")


def cmd_segs(table, args):
    bid, vm, foff, segs = find_kext(table, args.bundle)
    print(f"{bid}  header@{vm:#x} fileoff={foff:#x}")
    for name, vma, vmsz, fo, fs, prot in segs:
        print(f"  {name:20s} vm={vma:#x} vmsz={vmsz:#x} fileoff={fo:#x} "
              f"filesz={fs:#x} prot={prot}")


INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")


def _objdump(kc, start, stop):
    cmd = [OBJDUMP, "-D", "--arch=arm64", "--no-show-raw-insn",
           f"--start-address={start:#x}", f"--stop-address={stop:#x}", kc]
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def cmd_dis(table, args):
    addr = int(args.address, 0)
    # 4 bytes per instruction; ask for a generous window and trim to --count
    stop = addr + 4 * (args.count + 8)
    out = _objdump(args.kc, addr, stop)
    kept = []
    for line in out.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        kept.append(line.rstrip())
        if len(kept) >= args.count:
            break
    print("\n".join(kept))


def cmd_str(table, args):
    data = load_kc(args.kc)
    for a in args.addresses:
        addr = int(a, 0)
        hit = owner(table, addr, args.kc)
        if not hit:
            raise SystemExit(f"{addr:#x} not in any kext")
        foff = hit[4]
        end = data.index(b"\0", foff)
        print(f"{addr:#x}  {data[foff:end].decode('utf-8', 'replace')}")


PRINTABLE = set(range(0x20, 0x7f)) | {9, 10}


def cmd_strings(table, args):
    """Every NUL-terminated printable run inside one kext's __TEXT."""
    bid, vm, foff, segs = find_kext(table, args.bundle)
    data = load_kc(args.kc)
    text = [s for s in segs if s[0] == "__TEXT"]
    out = []
    for name, vma, vmsz, fo, fs, prot in text:
        base = owner(table, vma, args.kc)[4]
        blob = data[base:base + fs]
        i, n = 0, len(blob)
        while i < n:
            if blob[i] in PRINTABLE and blob[i] not in (9, 10):
                j = i
                while j < n and blob[j] in PRINTABLE and blob[j] != 0:
                    j += 1
                if j < n and j - i >= args.min_len:
                    out.append((vma + i, blob[i:j].decode("utf-8", "replace")))
                i = j
            i += 1
    for addr, s in out:
        if args.filter and args.filter.lower() not in s.lower():
            continue
        print(f"{addr:#x}  {s}")


def cmd_dump(table, args):
    """Raw bytes from any mapped kext address (segment-relative fileoffs)."""
    addr = int(args.address, 0)
    hit = owner(table, addr, args.kc)
    if not hit:
        raise SystemExit(f"{addr:#x} not in any kext")
    data = load_kc(args.kc)
    blob = data[hit[4]:hit[4] + args.length]
    if args.out:
        with open(args.out, "wb") as f:
            f.write(blob)
        print(f"{addr:#x} +{len(blob):#x} -> {args.out}")
    else:
        for i in range(0, len(blob), 16):
            row = blob[i:i + 16]
            print(f"{addr + i:#x}  " + " ".join(f"{b:02x}" for b in row))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kc", default=KC)
    ap.add_argument("--prelink", default=PRELINK_JSON)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list"); s.add_argument("--filter", default="")
    s = sub.add_parser("which"); s.add_argument("addresses", nargs="+")
    s = sub.add_parser("segs"); s.add_argument("bundle")
    s = sub.add_parser("dis")
    s.add_argument("address")
    s.add_argument("--count", type=int, default=40)
    s = sub.add_parser("str"); s.add_argument("addresses", nargs="+")
    s = sub.add_parser("dump"); s.add_argument("address")
    s.add_argument("--length", type=lambda v: int(v, 0), required=True)
    s.add_argument("--out", default="")
    s = sub.add_parser("strings"); s.add_argument("bundle")
    s.add_argument("--filter", default=""); s.add_argument("--min-len", type=int, default=4)
    args = ap.parse_args()
    table = kext_table(args.kc, args.prelink)
    {"list": cmd_list, "which": cmd_which, "segs": cmd_segs,
     "dis": cmd_dis, "str": cmd_str, "strings": cmd_strings, "dump": cmd_dump}[args.cmd](table, args)


if __name__ == "__main__":
    main()
