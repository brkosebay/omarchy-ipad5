#!/usr/bin/env python3
"""H.264 table generation for the Apple D5500 (Imagination MSVDX "VXD") VDEC.

Round 6.  Two *separate* artifacts were conflated by round 5 (§14 Q3):

  1.  THE CAVLC CODE TABLE.  One constant blob of 544 16-bit entries in the
      kext's __TEXT (0x5596d08, 0x440 bytes).  `DecodeCreateH264VLD` memcpy()s
      it once, at decoder-create time, into a 0x1000-byte, 8-byte-aligned GART
      buffer (kext ctx + 0x6338) that the command stream later publishes in ENT
      register 0x50c0 (flag 0x10 = "device address").  It is byte-identical to
      Imagination's `h264_vlc_table_data[]` in the open-source VXD driver.

  2.  THE SCALING-MATRIX AREA  S+0x000..S+0x181f  (S = ctx + sps*0x4898 + 0x518).
      Twenty 8-bit tables (6 x 16 entries, 14 x 64 entries) plus 8 DC words,
      generated once per SPS/PPS parse by 0x62a9344 (absent lists: defaults) or
      0x62a8e90 (present lists: parsed from the bitstream), then packed 4
      entries per 32-bit register into ENT 0x5800..0x5bfc -- *only* when
      0x13c378 bit 0 is set ("scaling matrices present").

Nothing here is per-slice and nothing here comes out of 0x62b6010: that
function builds the per-slice entropy blob, a different buffer.

Usage
    python3 tools/ds-video/vlc_tables.py --extract        # dump both artifacts
    python3 tools/ds-video/vlc_tables.py --default        # emit the S image
    python3 tools/ds-video/vlc_tables.py --regs           # 0x5800 block, default
    python3 tools/ds-video/vlc_tables.py --verify         # vs. img-vxe-vxd
    python3 tools/ds-video/vlc_tables.py --header         # GPL VLC-only header, offline

All kext offsets are unslid VAs of the iOS 16.7.10 iPad6,11 kernelcache build.
"""
import argparse
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MACHO = ROOT / 'build/video/kexts/AppleD5500.macho'
VXD_SRC = ROOT / 'build/img-vxe-vxd/decoder'

# ---------------------------------------------------------------- kext layout
VA_VLC_BLOB = 0xfffffff005596d08   # 544 x u16 packed CAVLC entries (0x440 B)
VLC_ENTRIES = 544
VA_SCL_A = 0xfffffff005595f34      # 64 u32: default intra 8x8 weighting
VA_SCL_B = 0xfffffff005596034      # 64 u32: default inter 8x8 weighting
SCL_ENTRIES = 64
# byte-gather permutations used when packing the 8-bit tables into registers
# (0x629ab2c: reg[i] = src[p[4i]] | src[p[4i+1]]<<8 | ... )
VA_PERM4 = 0xfffffff005595040      # n = 4,  16 entries  (4x4 tables)
VA_PERM16 = 0xfffffff005595050     # n = 16, 64 entries  (8x8 tables)

# ------------------------------------------------------- S area geometry (V)
# group g -> (entry count, number of lists)
SCL_GROUPS = ((16, 6), (64, 6), (64, 6), (64, 2))   # 4x4, 8x8, 16x16, 32x32
SCL_GROUP_STRIDE = 0x600     # bytes between groups
SCL_LIST_STRIDE = 0x100      # bytes between lists within a group
SCL_DC_BASE = 0x1800         # (g-2)*0x18 + 4*sub, groups 2 and 3 only
SCL_SIZE = 0x1820            # last word is S+0x181c


class MachO:
    def __init__(self, path):
        self.data = pathlib.Path(path).read_bytes()
        if struct.unpack_from('<I', self.data, 0)[0] != 0xfeedfacf:
            raise ValueError('not a 64-bit LE Mach-O')
        self.segs = []
        off = 32
        for _ in range(struct.unpack_from('<I', self.data, 16)[0]):
            cmd, sz = struct.unpack_from('<II', self.data, off)
            if cmd == 0x19:
                name = self.data[off + 8:off + 24].rstrip(b'\0').decode()
                vm, vs, fo, fs = struct.unpack_from('<QQQQ', self.data, off + 24)
                self.segs.append((name, vm, vs, fo, fs))
            off += sz

    def read(self, va, n):
        for _n, vm, _vs, fo, fs in self.segs:
            if vm <= va and va + n <= vm + fs:
                o = fo + (va - vm)
                return self.data[o:o + n]
        raise ValueError(f'{va:#x}+{n:#x} not file-backed')

    def words(self, va, n):
        return list(struct.unpack_from('<%dI' % n, self.read(va, 4 * n)))

    def halfs(self, va, n):
        return list(struct.unpack_from('<%dH' % n, self.read(va, 2 * n)))


# ------------------------------------------------------------ the two blobs
def load(macho=None):
    m = macho or MachO(MACHO)
    return {
        'vlc': m.halfs(VA_VLC_BLOB, VLC_ENTRIES),
        'scl_a': m.words(VA_SCL_A, SCL_ENTRIES),
        'scl_b': m.words(VA_SCL_B, SCL_ENTRIES),
        'perm4': list(m.read(VA_PERM4, 16)),
        'perm16': list(m.read(VA_PERM16, 64)),
    }


def scan_order(n):
    """The zig-zag scan FUN_2a2af7e4 builds at RBSP+8 (4x4) / RBSP+0x20 (8x8).

    Each anti-diagonal is walked from the bottom-left corner upwards, so this
    is the transpose of the JPEG/H.264 "classic" ordering:
        n = 4 -> 0, 4, 1, 8, 5, 2, 12, 9, 6, 3, 13, 10, 7, 14, 11, 15
    """
    out = []
    start = 0
    while len(out) < n * n:
        r, c, idx = 0, start, n * start
        while c >= 0:
            if r < n and c < n:
                out.append(idx)
            r += 1
            c -= 1
            idx += 1 - n
        start += 1
    return out


def default_scl_image(t):
    """Byte image of S+0x000..0x181f for "no scaling lists".

    Mirrors 0x62a9344 exactly: group 0 is filled sequentially with 16 (flat
    4x4); groups 1..3 take the intra table (lists 0..2) / inter table (lists
    3..5) placed at the zig-zag positions of the 8x8 scan; groups 2 and 3 also
    get a DC entry (groups > 1 only, as in HEVC's scaling_list_data()).
    """
    img = bytearray(SCL_SIZE)
    a, b = t['scl_a'], t['scl_b']
    for g, (cnt, nlist) in enumerate(SCL_GROUPS):
        for sub in range(nlist):
            base = g * SCL_GROUP_STRIDE + sub * SCL_LIST_STRIDE
            if g == 0:
                for i in range(cnt):
                    img[base + 4 * i] = 0x10
                continue
            if g <= 2:
                tab = a if sub < 3 else b
            else:
                tab = a if sub == 0 else b
            scan = scan_order(8)
            for i in range(cnt):
                # dest index = zig-zag position; the stored table is raster order
                img[base + 4 * scan[i]] = tab[i]
            if g >= 2:
                img[SCL_DC_BASE + (g - 2) * 0x18 + 4 * sub] = 0x10
    return bytes(img)


# --------------------------------------------- packing into ENT registers
def pack_table(scl, off, n):
    """0x629ab2c: n registers, 4 table bytes each, in Apple's gather order."""
    src = struct.unpack_from('<%dI' % (4 * n), scl, off)
    perm = t_perm16 if n == 0x10 else t_perm4
    out = []
    for i in range(n):
        w = 0
        for k in range(4):
            v = src[perm[4 * i + k]]
            assert v <= 0xff, 'table entry must fit in 8 bits'
            w |= v << (8 * k)
        out.append(w)
    return out


def scl_regs(scl):
    """(reg, value) pairs in the exact order 0x62b6010 emits them."""
    plan = [
        (0x5800, 0x1200, 0x10), (0x5840, 0x1300, 0x10),
        (0x5880, 0x0c00, 0x10), (0x58c0, 0x0d00, 0x10),
        (0x5900, 0x0e00, 0x10), (0x5940, 0x0f00, 0x10),
        (0x5980, 0x1000, 0x10), (0x59c0, 0x1100, 0x10),
        (0x5a00, 0x0600, 0x10), (0x5a40, 0x0700, 0x10),
        (0x5a80, 0x0800, 0x10), (0x5ac0, 0x0900, 0x10),
        (0x5b00, 0x0a00, 0x10), (0x5b40, 0x0b00, 0x10),
        (0x5b80, 0x0000, 0x04), (0x5b90, 0x0100, 0x04),
        (0x5ba0, 0x0200, 0x04), (0x5bb0, 0x0300, 0x04),
        (0x5bc0, 0x0400, 0x04), (0x5bd0, 0x0500, 0x04),
    ]
    out = []
    for reg, off, n in plan:
        for i, v in enumerate(pack_table(scl, off, n)):
            out.append((reg + 4 * i, v))
    # 0x5be0/0x5be4 <- S+0x1818/0x181c ; 0x5be8..0x5bfc <- S+0x1800..0x1814
    s4 = struct.unpack_from('<8I', scl, SCL_DC_BASE)
    out.append((0x5be0, s4[6]))
    out.append((0x5be4, s4[7]))
    for i in range(6):
        out.append((0x5be8 + 4 * i, s4[i]))
    return out


def emit_regs(cb, regs):
    """0x000f0000-form register writes, no run merging (what Apple does)."""
    for reg, val in regs:
        cb.append(0x000f0000 | (reg & 0xffff))
        cb.append(val)


# ------------------------------------------------------------- self-checks
def verify_against_img_vxd(t):
    """Byte-compare the 544-entry blob with Imagination's open-source table."""
    hdr = VXD_SRC / 'h264_vlc.h'
    if not hdr.exists():
        return 'skipped (no build/img-vxe-vxd checkout)'
    text = hdr.read_text()
    body = text.split('{', 1)[1].split('};')[0]
    import re
    nums = [int(x) for x in re.findall(r'\d+', re.sub(r'/\*.*?\*/', '', body,
                                                       flags=re.S))]
    nums = nums[:3 * VLC_ENTRIES]
    ti = [(nums[3 * i] << 12) | (nums[3 * i + 1] << 9) | nums[3 * i + 2]
          for i in range(VLC_ENTRIES)]
    bad = [i for i in range(VLC_ENTRIES) if ti[i] != t['vlc'][i]]
    if bad:
        return f'FAIL: {len(bad)} of {VLC_ENTRIES} differ, first {bad[:8]}'
    return f'OK: {VLC_ENTRIES}/{VLC_ENTRIES} entries identical to h264_vlc.h'


def selftest(t):
    """Structural checks that do not need the kext to be re-read."""
    errs = []

    # 1. every packed entry fits the documented field split
    for i, e in enumerate(t['vlc']):
        if e >> 15:
            errs.append(f'entry {i}: bit15 set ({e:#06x})')
    # 2. the two gather permutations are permutations of their ranges
    for name, p in (('perm4', t['perm4']), ('perm16', t['perm16'])):
        if sorted(p) != list(range(len(p))):
            errs.append(f'{name} is not a permutation')
    # 3. all scaling defaults are 8-bit
    for name in ('scl_a', 'scl_b'):
        if any(v > 0xff for v in t[name]):
            errs.append(f'{name} has a value > 0xff')
    # 4. the two tables differ (they really are intra vs inter)
    if t['scl_a'] == t['scl_b']:
        errs.append('scl_a == scl_b')
    return errs


# ---------------------------------------------------------------- C header
def public_header():
    """The shipped header uses only pinned GPL input, never a Mach-O."""
    import runpy
    generator = runpy.run_path(str(ROOT / 'scripts/vdec/h264-tables.py'))
    return generator['render_header'](generator['packed'](), workspace=True)


t_perm4 = None    # filled by main(), used by pack_table()
t_perm16 = None


def main():
    global t_perm4, t_perm16
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', action='store_true')
    ap.add_argument('--default', action='store_true')
    ap.add_argument('--regs', action='store_true')
    ap.add_argument('--verify', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--header', action='store_true')
    a = ap.parse_args()
    if not any(vars(a).values()):
        a.extract = a.verify = a.regs = True

    if a.header:
        sys.stdout.write(public_header())
        return 0

    t = load()
    t_perm4 = t['perm4']
    t_perm16 = t['perm16']

    if a.extract:
        print(f'CAVLC table: {VLC_ENTRIES} entries, {VLC_ENTRIES*2} bytes')
        for i in range(0, 32, 8):
            print('  ' + ' '.join(f'{e:04x}' for e in t['vlc'][i:i + 8]))
        print(f'scl_a (intra): {t["scl_a"]}')
        print(f'scl_b (inter): {t["scl_b"]}')
        print(f'perm4: {t["perm4"]}')
    if a.default:
        img = default_scl_image(t)
        for off in range(0, len(img), 16):
            if any(img[off:off + 16]):
                print(f'S+{off:04x}: ' + ' '.join(f'{b:02x}' for b in img[off:off + 16]))
    if a.regs:
        regs = scl_regs(default_scl_image(t))
        print(f'ENT 0x5800 block, {len(regs)} registers, '
              f'{2*len(regs)} command words')
        for i in range(0, len(regs), 4):
            print('  ' + '  '.join(f'{r:04x}={v:08x}' for r, v in regs[i:i + 4]))
    if a.verify:
        print('img-vxe-vxd cross-check:', verify_against_img_vxd(t))
    if a.selftest:
        errs = selftest(t)
        print('selftest:', 'OK' if not errs else '; '.join(errs))
        if errs:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
