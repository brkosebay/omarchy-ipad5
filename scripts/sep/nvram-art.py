#!/usr/bin/env python3
"""Extract com.apple.System.sep.art from an Apple IONVRAM bank image.

The iPad keeps the SEP anti-replay token in the kernel-only NVRAM system
partition.  The image is the Apple/CHRP bank format used by XNU's IONVRAM:
a 32-byte Apple header ("nvram"), then 16-byte partition headers whose
length is in 16-byte blocks, then key=value\\0 entries.  Binary values use
the 0xff escape: 0xff <n> means (n & 0x7f) copies of 0x00, or of 0xff
when bit 7 is set.

Usage:
    scripts/sep/nvram-art.py nvram.bin -o build/sep/art.bin

where nvram.bin is a raw dump of the NVRAM namespace (normally 0x2000
bytes, `nvram-bank-size` in the device tree).
"""
import argparse
import struct
import sys


def unescape(data):
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        i += 1
        if b == 0xFF:
            if i >= len(data):
                break
            n = data[i]
            i += 1
            count = n & 0x7F
            out += (b"\xff" if (n & 0x80) else b"\x00") * count
        else:
            out.append(b)
    return bytes(out)


def partitions(image):
    # The Apple bank header is 32 bytes; CHRP partitions start at 0x20.
    # sig(1) cksum(1) len_in_16b(2) name(12) data[len*16-16]
    off = 0x20
    while off + 16 <= len(image):
        blocks = struct.unpack("<H", image[off + 2:off + 4])[0]
        if blocks == 0:
            break
        size = blocks * 16
        if off + size > len(image):
            break
        name = image[off + 4:off + 16].split(b"\0", 1)[0].decode("latin1")
        yield name, image[off + 16:off + size]
        off += size


def find_art(image):
    for name, data in partitions(image):
        for entry in data.split(b"\0"):
            if not entry or b"=" not in entry:
                continue
            key, value = entry.split(b"=", 1)
            if key == b"com.apple.System.sep.art":
                return name, unescape(value)
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="raw NVRAM bank dump (e.g. 0x2000 bytes)")
    ap.add_argument("-o", "--output", default="art.bin",
                    help="write the raw ART token here (default: art.bin)")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    part, art = find_art(image)
    if art is None:
        print("com.apple.System.sep.art not found", file=sys.stderr)
        return 1

    with open(args.output, "wb") as f:
        f.write(art)
    print(f"{part}: com.apple.System.sep.art len={len(art)} "
          f"hex={art.hex()} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
