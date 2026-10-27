#!/usr/bin/env python3
"""Extract an arm64 Image[.gz] or m1n1 payload into a staging directory.

Never execute m1n1. Select the payload DTB by board compatible, preserve the
payload initramfs and command line. An explicit --dtb/--cmdline wins.
"""
import argparse
import gzip
import importlib.machinery
import struct
import zlib
from pathlib import Path

FDT = importlib.machinery.SourceFileLoader(
    "fdt", str(Path(__file__).parent / "device/linux-a9-fdt-merge")).load_module()
MAGIC = b"ARM\x64"
LIMIT = 256 * 1024 * 1024


def image(data):
    if len(data) < 64 or data[56:60] != MAGIC:
        return False
    size, flags = struct.unpack_from("<QQ", data, 16)
    # image_size includes BSS, which objcopy does not put in the file.
    if not 64 <= size <= LIMIT or flags & 1:
        raise ValueError("invalid size or big-endian arm64 Image")
    return True


def extract(data, board):
    if image(data):
        return data, None, None, None
    if data.startswith(b"\x1f\x8b"):
        plain = gzip.decompress(data)
        if image(plain):
            return plain, None, None, None
        raise ValueError("gzip input is not an arm64 kernel")
    # The payload has uncompressed DTBs, then one gzip kernel and one initrd.
    dtbs, kernels, initrds = [], [], []
    pos = 0
    while True:
        pos = data.find(b"\xd0\x0d\xfe\xed", pos)
        if pos < 0:
            break
        try:
            size = struct.unpack_from(">I", data, pos + 4)[0]
            if not 40 <= size <= len(data) - pos:
                raise ValueError("bad FDT size")
            blob = data[pos:pos + size]
            tree, _, _ = FDT.parse(blob)
            if board.encode() in tree.props.get("compatible", b"").split(b"\0"):
                dtbs.append((pos, blob))
        except (ValueError, IndexError, struct.error, UnicodeError):
            pass
        pos += 4
    if len(dtbs) != 1:
        raise ValueError(f"payload needs exactly one DTB for {board}, found {len(dtbs)}")
    pos = dtbs[0][0] + len(dtbs[0][1])
    while pos < len(data):
        start = data.find(b"\x1f\x8b\x08", pos)
        if start < 0:
            break
        d = zlib.decompressobj(31)
        try:
            plain = d.decompress(data[start:], LIMIT + 1)
            if not d.eof or len(plain) > LIMIT:
                raise ValueError("invalid or oversized gzip member")
            end = len(data) - len(d.unused_data)
            if image(plain):
                kernels.append(plain)
            elif plain.startswith((b"070701", b"070702")):
                initrds.append(plain)
            pos = end
        except (ValueError, zlib.error):
            pos = start + 3
    if len(kernels) != 1 or len(initrds) != 1:
        raise ValueError(f"payload needs one kernel and one initramfs; found {len(kernels)}/{len(initrds)}")
    prefix = data[:dtbs[0][0]]
    start = prefix.rfind(b"chosen.bootargs=")
    if start < 0:
        raise ValueError("payload has no chosen.bootargs")
    cmdline = prefix[start + 16:].split(b"\n", 1)[0].decode()
    return kernels[0], dtbs[0][1], gzip.compress(initrds[0], mtime=0), cmdline


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("out", type=Path)
    p.add_argument("--board", default="apple,j71t")
    p.add_argument("--dtb", type=Path)
    p.add_argument("--initrd", type=Path)
    p.add_argument("--cmdline")
    a = p.parse_args()
    kernel, dtb, initrd, cmdline = extract(a.input.read_bytes(), a.board)
    if a.dtb:
        dtb = a.dtb.read_bytes()
    if dtb:
        tree, _, _ = FDT.parse(dtb)
        if a.board.encode() not in tree.props.get("compatible", b"").split(b"\0"):
            p.error(f"DTB is not for {a.board}")
    if a.initrd:
        initrd = a.initrd.read_bytes()
    if a.cmdline is not None:
        cmdline = a.cmdline
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "Image.gz").write_bytes(gzip.compress(kernel, mtime=0))
    if dtb:
        (a.out / "dtb").write_bytes(dtb)
    if initrd:
        (a.out / "initramfs.gz").write_bytes(initrd)
    if cmdline:
        if "\0" in cmdline or "\n" in cmdline:
            p.error("command line must be a single line")
        (a.out / "cmdline").write_text(cmdline + "\n")
    (a.out / "SOURCE").write_text(str(a.input.resolve()) + "\n")


if __name__ == "__main__":
    main()
