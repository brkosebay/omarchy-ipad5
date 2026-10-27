#!/usr/bin/env python3
"""Merge several initramfs images into one single-gzip-stream initramfs.

  scripts/merge-initramfs.py OUT.gz BASE.gz EXTRA.gz [EXTRA.gz ...]

Each workstream builds its own initramfs (prebuilt base + appended newc
archives). BASE is copied whole; from each EXTRA only entries that BASE
or an earlier EXTRA doesn't already hold with identical content are
appended, in one trailing newc archive. Later inputs win on conflicting
content, as the kernel's unpacker overwrites. Output is a single gzip
stream, because m1n1 unpacks each gzip member as a separate initrd.
"""
import gzip
import sys


def entries(data):
    """Yield (name, header, body) for every newc entry in data."""
    i = 0
    while i < len(data):
        if data[i:i + 6] != b"070701":
            i += 1
            continue
        hdr = data[i:i + 110]
        fsize = int(hdr[54:62], 16)
        nsize = int(hdr[94:102], 16)
        name = data[i + 110:i + 110 + nsize - 1].decode(errors="replace")
        j = (i + 110 + nsize + 3) & ~3
        yield name, hdr, data[j:j + fsize]
        i = (j + fsize + 3) & ~3


def norm(name):
    return name[2:] if name.startswith("./") else name


def newc(name, hdr, body, ino):
    # Fresh inode number and nlink 1: the kernel links newc entries that
    # share an inode number, and inputs reuse them across archives.
    name_b = name.encode() + b"\0"
    hdr = (hdr[:6] + b"%08X" % ino + hdr[14:38] + b"%08X" % 1 + hdr[46:94]
           + b"%08X" % len(name_b) + hdr[102:])
    out = hdr + name_b
    out += b"\0" * (-len(out) % 4)
    out += body + b"\0" * (-len(body) % 4)
    return out


def main():
    out, base, *extras = sys.argv[1:]
    base_data = gzip.open(base).read()
    seen = {}
    for name, _, body in entries(base_data):
        if name != "TRAILER!!!":
            seen[norm(name)] = body
    tail = b""
    added = []
    for path in extras:
        for name, hdr, body in entries(gzip.open(path).read()):
            n = norm(name)
            if name == "TRAILER!!!" or seen.get(n) == body:
                continue
            seen[n] = body
            tail += newc(n, hdr, body, 0x7a90000 + len(seen))
            if body:
                added.append(n)
    tail += newc("TRAILER!!!", b"070701" + b"0" * 104, b"", 0)
    tail += b"\0" * (-len(tail) % 512)
    with gzip.GzipFile(out, "wb", 9, mtime=0) as f:
        f.write(base_data)
        f.write(tail)
    print(f"{out}: base {base} + {len(added)} files: {' '.join(added)}")


if __name__ == "__main__":
    main()
