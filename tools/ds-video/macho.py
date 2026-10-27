#!/usr/bin/env python3
"""Minimal read-only Mach-O helper for the AppleD5500 kext.

The kext is a 64-bit little-endian Mach-O with unslid VAs of the form
0xfffffff0xxxxxxxx. This module maps an unslid VA to a file offset so the
constant tables Ghidra shows as `DAT_fffffff00559xxxx` can be read straight
out of the file (no Ghidra session needed).

    python3 tools/ds-video/macho.py --segments
    python3 tools/ds-video/macho.py --scan-perm 64
"""
import argparse
import struct
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
MACHO = ROOT / 'build/video/kexts/AppleD5500.macho'


class MachO:
    def __init__(self, path=MACHO):
        self.data = pathlib.Path(path).read_bytes()
        magic = struct.unpack_from('<I', self.data, 0)[0]
        if magic != 0xfeedfacf:
            raise ValueError(f'not a 64-bit LE Mach-O: {magic:#x}')
        self.segs = []
        ncmds = struct.unpack_from('<I', self.data, 16)[0]
        off = 32
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from('<II', self.data, off)
            if cmd == 0x19:                     # LC_SEGMENT_64
                name = self.data[off + 8:off + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                    '<QQQQ', self.data, off + 24)
                self.segs.append((name, vmaddr, vmsize, fileoff, filesize))
            off += sz

    def read(self, va, n):
        for name, vmaddr, vmsize, fileoff, filesize in self.segs:
            if vmaddr <= va and va + n <= vmaddr + filesize:
                o = fileoff + (va - vmaddr)
                return self.data[o:o + n]
        raise ValueError(f'{va:#x}+{n:#x} not in a file-backed segment')

    def u32(self, va, n=1):
        b = self.read(va, 4 * n)
        return list(struct.unpack_from('<%dI' % n, b)) if n > 1 else \
            struct.unpack_from('<I', b)[0]

    def va_of_offset(self, fileoff):
        for name, vmaddr, vmsize, fo, filesize in self.segs:
            if fo <= fileoff < fo + filesize:
                return vmaddr + (fileoff - fo)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--segments', action='store_true')
    ap.add_argument('--dump', nargs=2, metavar=('VA', 'LEN'))
    ap.add_argument('--scan-perm', type=int, metavar='N',
                    help='find N-word arrays that are a permutation of 0..N-1')
    a = ap.parse_args()
    m = MachO()
    if a.segments:
        for s in m.segs:
            print(f'{s[0]:14s} vm={s[1]:#x} vsize={s[2]:#x} '
                  f'file={s[3]:#x} size={s[4]:#x}')
    if a.dump:
        va, n = int(a.dump[0], 0), int(a.dump[1], 0)
        b = m.read(va, n)
        for i in range(0, len(b), 16):
            row = b[i:i + 16]
            print(f'{va+i:016x}  ' + ' '.join(f'{x:02x}' for x in row))
    if a.scan_perm:
        n = a.scan_perm
        want = list(range(n))
        for name, vmaddr, vmsize, fileoff, filesize in m.segs:
            b = m.data[fileoff:fileoff + filesize]
            for i in range(0, len(b) - 4 * n, 4):
                w = struct.unpack_from('<%dI' % n, b, i)
                if sorted(w) == want:
                    print(f'perm[{n}] at {vmaddr + i:#x} (file {fileoff + i:#x})')


if __name__ == '__main__':
    main()
