#!/usr/bin/env python3
"""Find IMG4 containers in the APFS container and report their payload type.

Read-only by construction: the block device is opened O_RDONLY and read in
bounded chunks. Nothing is mapped into the page cache in one go and nothing
is written.

The full container is 12 GiB and scanning it puts real pressure on a 2 GiB
device (two earlier runs coincided with the iPad dropping off USB), so:

  * default: scan only around the last known offset (SEP_OFF, 8 MiB window)
  * --scan:  walk the whole container in 4 MiB chunks with 64 KiB overlap,
             stopping at the first "sepi" IMG4 unless --all is given

Usage:
  find-img4.py /dev/nvme0n1p1                 # known-offset window
  find-img4.py /dev/nvme0n1p1 --scan --extract sepi out.img4
"""
import argparse
import os
import sys

CHUNK = 4 << 20
OVERLAP = 64 << 10

# Last known location of the personalized sep-firmware IMG4 on 20H350/j71t.
SEP_OFF = 0x00E0CE0000
SEP_LEN = 3414298
WINDOW = 8 << 20


def parse_at(buf, off):
    """Return (total_len, payload_type) for an IMG4 starting at off, or None."""
    if off + 32 > len(buf):
        return None
    if buf[off] != 0x30 or buf[off + 1] not in (0x81, 0x82, 0x83):
        return None
    if buf[off + 5:off + 7] != b"\x16\x04" or buf[off + 7:off + 11] != b"IMG4":
        return None
    n = buf[off + 1]
    if n == 0x83:
        ln, hdr = int.from_bytes(buf[off + 2:off + 5], "big"), 5
    elif n == 0x82:
        ln, hdr = int.from_bytes(buf[off + 2:off + 4], "big"), 4
    else:
        ln, hdr = buf[off + 2], 3
    k = buf.find(b"IM4P", off, min(off + 96, len(buf)))
    typ = buf[k + 6:k + 10].decode("latin1") if k >= 0 else "??"
    return hdr + ln, typ


def scan_window(fd, start, length):
    start &= ~0xFFF
    buf = os.pread(fd, length, start)
    pos, hits = 0, []
    while True:
        j = buf.find(b"IMG4", pos)
        if j < 0:
            break
        pos = j + 4
        p = parse_at(buf, j - 7)
        if p:
            hits.append((start + j - 7, p[0], p[1]))
    return hits


def scan_full(fd, size, want, stop_early):
    pos, hits, carry, seen = 0, [], b"", set()
    while pos < size:
        buf = carry + os.pread(fd, CHUNK, pos)
        base = pos - len(carry)
        i = 0
        while True:
            j = buf.find(b"IMG4", i)
            if j < 0:
                break
            i = j + 4
            p = parse_at(buf, j - 7)
            if p:
                off = base + j - 7
                if off not in seen:
                    seen.add(off)
                    hits.append((off, p[0], p[1]))
                if stop_early and want and p[1] == want:
                    return hits
        carry = buf[-OVERLAP:]
        pos += CHUNK
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        except OSError:
            pass
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dev")
    ap.add_argument("--scan", action="store_true",
                    help="walk the whole container instead of the known window")
    ap.add_argument("--all", action="store_true",
                    help="with --scan: do not stop at the first match")
    ap.add_argument("--extract", nargs=2, metavar=("TYPE", "OUT"))
    args = ap.parse_args()

    fd = os.open(args.dev, os.O_RDONLY)
    size = os.lseek(fd, 0, os.SEEK_END)
    want = args.extract[0].encode() if args.extract else None

    if args.scan:
        print("scanning %s (%d bytes, chunked)" % (args.dev, size))
        hits = scan_full(fd, size, want, stop_early=not args.all)
    else:
        start = max(0, SEP_OFF - WINDOW // 2)
        print("scanning %s window %#x..%#x (use --scan for the whole device)"
              % (args.dev, start, start + WINDOW))
        hits = scan_window(fd, start, min(WINDOW, size - start))

    for off, ln, typ in hits:
        print("%#012x  %9d  %s" % (off, ln, typ))

    if args.extract:
        cand = [h for h in hits if h[2] == want and h[1] > 100000]
        if not cand:
            sys.exit("no %r IMG4 found" % want)
        off, ln, _ = cand[0]
        data = os.pread(fd, ln, off)
        with open(args.extract[1], "wb") as f:
            f.write(data)
        print("wrote %s (%d bytes) from %#x" % (args.extract[1], len(data), off))

    os.close(fd)


if __name__ == "__main__":
    main()
