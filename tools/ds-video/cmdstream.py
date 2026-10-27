#!/usr/bin/env python3
"""Read the AppleD5500 H.264 command generators out of the decompilation dump.

Two views, both in source order:

  --emit   the actual command-stream writes: emitter-helper calls and raw
           word stores (what leaves for the hardware / firmware), with the
           value expression.
  --slots  for each ctx bitfield slot (ctx+0x13cXXX) the ordered list of
           (shift, source-expression) that were OR-ed into it, i.e. the
           bit layout of every command word.

  python3 tools/ds-video/cmdstream.py --emit FUN_fffffff0062b38d0

  --gen IN.json [--text] independently generates the AVC recipe in document
  section 16.3 onward (the older ctx+0x13cxxx recipe is HEVC!). It needs no dump,
  Ghidra session, driver tree, device, or third-party Python modules.

Input JSON (one picture per file; integer values, or 0x-prefixed strings):
  {"sps": {v4l2_ctrl_h264_sps fields},
   "pps": {v4l2_ctrl_h264_pps fields},
   "decode_params": {v4l2_ctrl_h264_decode_params fields,
       "dpb": [16 x {"reference_ts", "pic_num", "frame_num", "fields",
                     "top_field_order_cnt", "bottom_field_order_cnt", "flags"}]},
   "slices": [{"params": {v4l2_ctrl_h264_slice_params fields,
       "ref_pic_list0": [{"fields", "index"}], "ref_pic_list1": [...]},
       "offset": NAL-header byte offset in OUTPUT (after any start code),
       "size": NAL byte count including that header}],
   "first_picture": bool,
   "va": {"cmd", "src", "dst_luma", "dst_chroma", "dst_stride",
          "dpb": [16 x {"luma", "chroma"} or null],
          "cavlc", "intra", "stream_state", "colocated"}}

V4L2 flags are numeric UAPI bit masks. header_bit_size measures EBSP bits from
the NAL header to slice_data, including header EPBs; CABAC must include its
alignment bits. src+offset is the NAL header VA. Optional va.dst_chroma_stride
defaults to dst_stride. Optional va.colocated may instead be one VA per slice
(normally the same per-CAPTURE buffer VA). va.cmd is required: packet +0x10 is
the command chunk's device VA, not merely its offset. reference_ts is resolved
by the driver; this generator takes already resolved va.dpb[index] addresses.

Output JSON: {"cmd": [u32], "packets": [{"kind", "payload": [30 u32]}],
"unresolved": [{"field", "reason"}], "complete": bool, "recipe": str}.
Unknown required values are zero and reported. No host packet envelopes enter
cmd; normal AVC chunks concatenate without padding and there is no kind-4
packet. --text prints each word with its byte offset and decoded meaning.
Scope: progressive 8-bit 4:2:0 I/P, flat scaling, one reconstruction output,
no weights/FMO/second deblock pass. Unsupported controls are explicitly marked
in unresolved; such output is diagnostic, never a complete reference vector.
CABAC scalar fields are generated but explicitly marked unvalidated. See §16.
"""
import argparse
import json
import re
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEC = ROOT / 'build/video/d5500-decomp.txt'

# emitter library in the 0x629a34c..0x629b15c range, decoded in the doc
HELPERS = {
    'FUN_fffffff00629a34c': '0e-XFER',
    'FUN_fffffff00629a444': '0a-ADDR',
    'FUN_fffffff00629a4c8': 'BEGIN',
    'FUN_fffffff00629a568': '0x5-WORD',
    'FUN_fffffff00629a610': '0x5-WORD2',
    'FUN_fffffff00629a6c0': 'RAW',
    'FUN_fffffff00629a760': 'COPY-U32',
    'FUN_fffffff00629a81c': 'PATCHSIZE',
    'FUN_fffffff00629a91c': 'END',
    'FUN_fffffff00629a9b4': 'BEGIN-F',
    'FUN_fffffff00629aa54': 'F-REG0',
    'FUN_fffffff00629aa5c': '0xf-REG',
    'FUN_fffffff00629ab2c': '0xf-REGBLK',
    'FUN_fffffff00629ac54': 'PAIR',
    'FUN_fffffff00629ad38': 'PAIRF',
    'FUN_fffffff00629ae20': 'PLANE',
    'FUN_fffffff00629af50': 'END2',
    'FUN_fffffff00629afb4': 'GETCOUNT',
    'FUN_fffffff00629afbc': 'GETPTR',
    'FUN_fffffff00629afc4': 'BEGIN-REG',
    'FUN_fffffff00629b068': '0x1-REG',
    'FUN_fffffff00629b15c': 'END-REG',
    'FUN_fffffff0062982f8': 'PACKET',
    'FUN_fffffff0062b2344': 'SCALING',
    'FUN_fffffff0062c5fb8': 'AVC-PREP',
    'FUN_fffffff0062c6564': 'AVC-FE',
    'FUN_fffffff0062c66b0': 'AVC-RENDEC',
    'FUN_fffffff0062c4788': 'AVC-OUTPUT',
    'FUN_fffffff0062c44b4': 'AVC-DEBLOCK',
}


def functions(lines):
    out, order, cur = {}, [], None
    for i, l in enumerate(lines):
        m = re.match(r'/\* ===== ([0-9a-f]+) (\S+) \((\d+) bytes\)', l)
        if m:
            cur = m.group(2)
            out[cur] = {'addr': m.group(1), 'lines': []}
            order.append(cur)
        elif cur is not None:
            out[cur]['lines'].append(l)
    return out, order


def emit_view(name, f):
    print(f'\n===== {name} ({f["addr"]}) : command-stream emission order =====')
    # Decompiler calls often put both '(' and all arguments on following lines.
    # Join only statements, retaining source order and the legacy raw-store view.
    body = '\n'.join(f['lines']).partition('{')[2]
    statements = re.sub(r'\s+', ' ', body).split(';')
    for l in statements:
        s = l.strip() + ';'
        for fn, tag in HELPERS.items():
            match = re.search(re.escape(fn) + r'\s*(\(.*)', s)
            if match:
                print(f'  {tag:12s} {match.group(1)}')
                break
        else:
            # direct stores into the command buffer: puVarN / *(u32**)(..+0x88)
            if re.search(r'= 0x[0-9a-f]{8};', s) and ('puVar' in s or 'uVar' in s[-20:]):
                print(f'  EMITWORD   {s}')
            elif re.search(r'\w+\[[0-9]\] = ', s) and 'param_1' not in s:
                print(f'  EMITWORD   {s}')


def slot_view(name, f):
    print(f'\n===== {name} ({f["addr"]}) : ctx bitfield slots =====')
    slots = {}
    width = {}
    for l in f['lines']:
        s = l.strip()
        m = re.match(r'if \((\w*) ?<? ?(0x[0-9a-f]+|[\d]+) < (\w+)\)', s)
        if m:
            width[m.group(3)] = m.group(2)
        m = re.match(r'\*\(\w+ \*\)\(param_1 \+ (0x[0-9a-f]+)\) = (\w+) \| (.*);', s)
        if m:
            e = m.group(3)
            sh = re.search(r'<< (0x[0-9a-f]+)', e)
            slots.setdefault(m.group(1), []).append(
                (int(sh.group(1), 16) if sh else None, e))
            continue
        m = re.match(r'\*\(\w+ \*\)\(param_1 \+ (0x[0-9a-f]+)\) = (.*);', s)
        if m and ('<<' in m.group(2) or '|' in m.group(2)):
            e = m.group(2)
            sh = re.search(r'<< (0x[0-9a-f]+)', e)
            slots.setdefault(m.group(1), []).append(
                (int(sh.group(1), 16) if sh else None, e))
    for k in sorted(slots, key=lambda x: int(x, 16)):
        print(f'  {k}:')
        for sh, e in slots[k]:
            print(f'      bit {sh if sh is not None else "?":>4}  {e[:110]}')


VLC_REG_VALUES = (
    0x26000, 0x738a0, 0x828f4, 0xa312d, 0xb5959, 0xc517b,
    0xd1196, 0xdb1ad, 0xe21be, 0xe59c8, 0xe79cd, 0xeb1d3,
    0xed1d8, 0xf09dd, 0xf71e7, 0x1041f6, 0x1256a4dd,
    0x01489292, 0x11248050, 0x22a2, 0x2a02, 0x8108282a, 0x12,
)
VLC_PAIRS = tuple((0x60ec + 4*i, v) for i, v in enumerate(VLC_REG_VALUES)) + (
    (0x61c0, 0x10920e), (0x61c4, 0x10d216), (0x61c8, 0x10f21c),
)


def number(value):
    """Accept JSON integers and explicit hexadecimal strings, never floats."""
    if isinstance(value, str):
        return int(value, 0)
    if not isinstance(value, int):
        raise ValueError(f'expected integer, got {value!r}')
    return value


class AVCReference:
    """Independent transcription of 62c4c98/62c6564/62c66b0, §16.4.

    This models final submitted bytes (including patches to CTRL_ALLOC_HEADER),
    not the host's intermediate write history. No HEVC fields are substituted.
    """

    def __init__(self, job):
        self.job = job
        self.cmd, self.notes, self.packets, self.unresolved = [], [], [], []
        self._reported = set()
        self.va = job.get('va', {})

    def unknown(self, field, reason):
        if (field, reason) not in self._reported:
            self._reported.add((field, reason))
            self.unresolved.append({'field': field, 'reason': reason})
        return 0

    def get(self, obj, key, path, default=None):
        if key not in obj or obj[key] is None:
            if default is not None:
                return default
            return self.unknown(path + '.' + key, 'missing input; emitted 0')
        return number(obj[key])

    def addr(self, key):
        value = self.get(self.va, key, 'va')
        if not 0 <= value <= 0xffffffff:
            raise ValueError(f'va.{key} must fit the 32-bit command address field')
        return value

    def word(self, value, meaning):
        self.cmd.append(value & 0xffffffff)
        self.notes.append(meaning)

    def r1(self, pairs):
        """One BEGIN/END group; merge only adjacent register addresses."""
        header, next_reg, count = None, None, 0
        for reg, value, name in pairs:
            if reg != next_reg:
                header, count = len(self.cmd), 1
                self.word(0x10010000 | reg, f'REGVALPAIR {reg:04x}, count=1')
            else:
                count += 1
                self.cmd[header] += 0x10000
                self.notes[header] = f'REGVALPAIR {self.cmd[header]&0xffff:04x}, count={count}'
            self.word(value, f'{reg:04x} {name}')
            next_reg = reg + 4

    def r5(self, reg, values, name, flags=0):
        self.word(0x50000000 | flags | (len(values) << 16) | reg,
                  f'RENDEC_BLOCK {reg:04x}, count={len(values)}, flags={flags:08x} {name}')
        for i, value in enumerate(values):
            self.word(value, f'{reg+4*i:04x} {name}[{i}]')

    def reference_state(self, dec):
        dpb = dec.get('dpb', [])
        if len(dpb) != 16:
            self.unknown('decode_params.dpb', 'expected 16 entries; missing entries treated inactive')
        if len(dpb) > 16:
            raise ValueError('decode_params.dpb exceeds 16 entries')
        dpb = list(dpb) + [{} for _ in range(16-len(dpb))]
        active = [i for i, d in enumerate(dpb) if number(d.get('flags', 0)) & 2]
        highwater = max(active, default=-1) + 1
        longterm = sum(1 << i for i in active if number(dpb[i].get('flags', 0)) & 4)
        return dpb, set(active), highwater, longterm

    def list_bytes(self, params, list_id, count, active, path):
        values = [0xff] * 32
        refs = params.get(f'ref_pic_list{list_id}', [])
        for i in range(count):
            if i >= len(refs):
                # Unknown is zero in the command, rather than an invented ref.
                values[i] = self.unknown(path + f'.ref_pic_list{list_id}[{i}]',
                                         'missing active reference index; emitted 0')
                continue
            ref = refs[i]
            index = self.get(ref, 'index', path + f'.ref_pic_list{list_id}[{i}]')
            fields = self.get(ref, 'fields', path + f'.ref_pic_list{list_id}[{i}]')
            if not 0 <= index < 16:
                raise ValueError(f'{path} reference index {index} outside dpb[16]')
            if index not in active:
                self.unknown(path + f'.ref_pic_list{list_id}[{i}]', 'reference points to inactive DPB entry')
            if fields != 3:
                self.unknown(path + f'.ref_pic_list{list_id}[{i}].fields',
                             'field references are outside the verified progressive recipe')
            values[i] = index | (0x80 if fields == 2 else 0)
        return values

    @staticmethod
    def pack_bytes(values):
        values = list(values)
        return [sum(b << (8*k) for k, b in enumerate(values[i:i+4]))
                for i in range(0, len(values), 4)]

    def scaling_words(self, pf):
        """62c66b0 L39756-39766 + 62be538 L33392-33534 (round 6).

        D[63f8] holds the six 4x4 lists at +0x00..0x50 and the 8x8 lists at
        +0x60/+0xa0, all in raster order (the parser 629c918 stores
        list[zigzag[j]]; the defaults at 0xfffffff005596170 are raster);
        flat 16 unless a scaling matrix is present, and the 8x8 lists flat
        unless transform_8x8_mode_flag.  V4L2's control is raster too.
        """
        flat = [16] * 224
        if not pf & 0x80:
            return self.pack_bytes(flat)
        m = self.job.get('scaling_matrix')
        if not m:
            self.unknown('scaling_matrix', 'SCALING_MATRIX_PRESENT without the control; emitted flat')
            return self.pack_bytes(flat)
        l4 = [[number(v) for v in row] for row in m['scaling_list_4x4']]
        l8 = [[number(v) for v in row] for row in m['scaling_list_8x8']]
        if any(len(r) != 16 for r in l4) or any(len(r) != 64 for r in l8) or len(l4) != 6 or len(l8) < 2:
            raise ValueError('scaling_matrix needs 6x16 and 6x64 lists')
        out = (l8[1] + l8[0]) if pf & 0x40 else [16] * 128
        for i in (0, 3, 4, 1, 5, 2):
            out += l4[i]
        if any(not 1 <= v <= 255 for v in out):
            raise ValueError('scaling factors must be 1..255')
        return self.pack_bytes(out)

    def b_chunks(self, dec, dpb, active, highwater, lists, l1, va_dpb, path):
        """62c66b0 L39776-39887 (round 6): POCs, co-located picture, L1."""
        def poc(v):
            v &= 0xffffffff
            return (v >> 15 & 0x10000) | (v & 0xffff)
        top = self.get(dec, 'top_field_order_cnt', 'decode_params', 0)
        bot = self.get(dec, 'bottom_field_order_cnt', 'decode_params', 0)
        words = [poc(top), poc(bot)]
        for di in range(highwater):
            d = dpb[di]
            if di in active:
                words += [poc(self.get(d, 'top_field_order_cnt', f'decode_params.dpb[{di}]', 0)),
                          poc(self.get(d, 'bottom_field_order_cnt', f'decode_params.dpb[{di}]', 0))]
            else:
                words += [0, 0]     # [I] psb writes 0 for unused slots
        self.r5(0x6250, words, 'BE_FOC0/FOC1, TOP/BOT_FOC per DPB slot')
        col = lists[1][0]
        address = va_dpb[col & 0x7f] if col != 0xff and (col & 0x7f) < len(va_dpb) else None
        if address is None or 'colocated' not in address:
            col_pic0, col_va = 0xdeadbeef, 0xdeadbeef
            self.unknown(path + '.colocated', 'RefPicList1[0] has no stored-MB buffer VA')
        else:
            d = dpb[col & 0x7f]
            cur = min(top, bot)
            dtop = abs(cur - self.get(d, 'top_field_order_cnt', 'dpb', 0))
            dbot = abs(cur - self.get(d, 'bottom_field_order_cnt', 'dpb', 0))
            # frame co-located picture: NOTFRAME bit4 and MBAFF bit2 clear
            col_pic0, col_va = int(dbot <= dtop), self.get(address, 'colocated', 'va.dpb')
        inverse = [0xff] * 32
        for i in range(31, -1, -1):
            b = lists[0][i]
            if b != 0xff:
                inverse[(b >> 3 & 0x10) | (b & 0x1f)] = i
        self.r5(0x62f4, [col_pic0, col_va] + self.pack_bytes(inverse) +
                self.pack_bytes(lists[1][:(l1 // 4 + 1) * 4]),
                'BE_COL_PIC0, BASE_ADDR_COL_PIC, LIST0_INVERSE[8], LIST1')

    def weight_words(self, pw, li, n, refs, path):
        """62c66b0 L40092-40129, values copied by 62be288 (zero for 0xff refs)."""
        fac, off = [0] * 32, [0] * 32
        if pw:
            wf = pw.get('weight_factors', [{}, {}])[li]
            for i in range(n + 1):
                if refs[i] == 0xff:
                    continue
                lw = number(wf['luma_weight'][i])
                lo = number(wf['luma_offset'][i])
                cw = [number(v) for v in wf['chroma_weight'][i]]
                co = [number(v) for v in wf['chroma_offset'][i]]
                fac[i] = (cw[1] & 0x1ff) << 18 | (cw[0] & 0x1ff) << 9 | (lw & 0x1ff)
                off[i] = (co[1] & 0xff) << 16 | (co[0] & 0xff) << 8 | (lo & 0xff)
        return fac + off

    def run(self):
        sps, pps = self.job.get('sps', {}), self.job.get('pps', {})
        dec = self.job.get('decode_params', {})
        sf = self.get(sps, 'flags', 'sps')
        pf = self.get(pps, 'flags', 'pps')
        df = self.get(dec, 'flags', 'decode_params')
        wmb = self.get(sps, 'pic_width_in_mbs_minus1', 'sps') + 1
        map_rows = self.get(sps, 'pic_height_in_map_units_minus1', 'sps') + 1
        frame_only, mbaff = bool(sf & 16), bool(sf & 32) and not bool(df & 2)
        hmb = map_rows * (1 if frame_only else 2)
        field, bottom = bool(df & 2), bool(df & 4)
        ph = hmb >> field
        chroma = self.get(sps, 'chroma_format_idc', 'sps')
        depth_y = self.get(sps, 'bit_depth_luma_minus8', 'sps')
        depth_c = self.get(sps, 'bit_depth_chroma_minus8', 'sps')
        level = self.get(sps, 'level_idc', 'sps')
        if not frame_only or field or mbaff:
            self.unknown('sps/decode_params.flags', 'interlaced/MBAFF not validated; output is diagnostic')
        if chroma != 1 or depth_y or depth_c:
            self.unknown('sps.chroma/bit_depth', 'only 8-bit 4:2:0 reference recipe is validated')
        if not 1 <= wmb <= 256 or not 1 <= ph <= 256 or wmb*ph > 0x4000:
            raise ValueError('picture exceeds the AVC register bit widths')
        if self.get(pps, 'num_slice_groups_minus1', 'pps', 0):
            self.unknown('pps.num_slice_groups_minus1', 'FMO unsupported; SGM base/map are missing')
        if pf & 1:
            self.unknown('pps.flags.ENTROPY_CODING_MODE', 'CABAC scalar recipe generated; context initialization not validated')
        profile_idc = self.get(sps, 'profile_idc', 'sps')
        if profile_idc == 244:
            self.unknown('sps.profile_idc', '4:4:4 special profile path not implemented')
        reference = bool(self.get(dec, 'nal_ref_idc', 'decode_params'))
        min_bipred8 = wmb > 80 and level > 30
        # 62be538 forces internal profile 3; 62c6564 maps it to control 0x11.
        # 62c4c98 L38955-39076, not psb's older profile selection.
        sps0 = ((chroma >> 1) & 1) << 28 | depth_c << 25 | depth_y << 22
        sps0 |= 0x7000 | min_bipred8 << 11 | bool(sf & 64) << 10
        sps0 |= frame_only << 8 | (chroma & 1) << 9 | (wmb-1)
        pps0 = (self.get(pps, 'second_chroma_qp_index_offset', 'pps', 0) & 31) << 24
        pps0 |= (self.get(pps, 'chroma_qp_index_offset', 'pps', 0) & 31) << 16
        bipred = self.get(pps, 'weighted_bipred_idc', 'pps', 0)
        pps0 |= bipred << 8 | self.get(pps, 'num_slice_groups_minus1', 'pps', 0) << 4
        pps0 |= bool(pf & 1) << 2 | bool(pf & 16) << 1 | bool(pf & 64)
        pic0 = (wmb*ph-1) << 16 | (ph-1) << 8
        pic0 |= reference << 3 | mbaff << 2 | field << 1 | bottom
        field_type = (1 if bottom else 0) if field else (3 if mbaff else 2)
        operating_mode = 0x10210000 | bool(chroma) << 12
        stride = self.addr('dst_stride')
        stride_c = self.get(self.va, 'dst_chroma_stride', 'va', stride)
        if stride < 16*wmb or stride_c < 16*wmb or stride % 64 or stride_c % 64:
            raise ValueError('NV12 strides must cover coded width and be multiples of 64')
        if stride > 0x3ffff or stride_c > 0xffff:
            raise ValueError('stride does not fit CTRL_ALLOC_HEADER / extended stride fields')
        dst_y, dst_c = self.addr('dst_luma'), self.addr('dst_chroma')
        cmd_va, src_va = self.addr('cmd'), self.addr('src')
        cavlc, intra = self.addr('cavlc'), self.addr('intra')
        stream_state = self.addr('stream_state')
        dpb, active, highwater, longterm = self.reference_state(dec)
        va_dpb = self.va.get('dpb', [])
        slices = self.job.get('slices', [])
        if not slices:
            raise ValueError('slices must contain at least one slice')
        for si, sl in enumerate(slices):
            path = f'slices[{si}]'
            p = sl.get('params', {})
            st = self.get(p, 'slice_type', path+'.params') % 5
            if st not in (0, 1, 2):
                raise ValueError('SP/SI slices have no verified recipe')
            code = (1, 2, 0)[st]
            first = self.get(p, 'first_mb_in_slice', path+'.params')
            if not 0 <= first < wmb*ph:
                raise ValueError('first_mb_in_slice outside picture')
            x, y = first % wmb, first // wmb
            if mbaff:
                y *= 2
            qp = 26 + self.get(pps, 'pic_init_qp_minus26', 'pps', 0) + self.get(p, 'slice_qp_delta', path+'.params', 0)
            if not 0 <= qp <= 51:
                raise ValueError('8-bit SliceQpY must be in 0..51')
            l0 = 0 if st == 2 else self.get(p, 'num_ref_idx_l0_active_minus1', path+'.params')
            l1 = 0 if st == 2 else self.get(p, 'num_ref_idx_l1_active_minus1', path+'.params')
            if not 0 <= l0 < 32 or not 0 <= l1 < 32:
                raise ValueError('reference counts must fit 5 bits')
            deblock = self.get(p, 'disable_deblocking_filter_idc', path+'.params', 0)
            alpha = self.get(p, 'slice_alpha_c0_offset_div2', path+'.params', 0) & 15
            beta = self.get(p, 'slice_beta_offset_div2', path+'.params', 0) & 15
            cabac_init = self.get(p, 'cabac_init_idc', path+'.params', 0)
            direct = bool(self.get(p, 'flags', path+'.params', 0) & 1)
            if deblock > 2 or not 0 <= cabac_init <= 2:
                raise ValueError('deblock_idc/cabac_init_idc outside syntax range')
            slice0 = direct << 31 | deblock << 28 | alpha << 24 | beta << 20
            slice0 |= field_type << 18 | code << 16 | cabac_init << 14 | si
            slice1 = y << 24 | x << 16 | qp << 10 | l1 << 5 | l0
            slice_params = bool(pf & 16) << 23 | (bool(pf & 4) | bipred << 1) << 17
            slice_params |= deblock << 12 | alpha << 8 | beta << 4 | field_type << 2 | code
            start = len(self.cmd) * 4
            header = [0x90000000, slice_params, 0, stride_c | stride_c << 16,
                      x | y << 16, (wmb-1) | (ph-1) << 16, 0, 0, 0,
                      depth_c << 12 | depth_y << 8 | chroma]
            for i, value in enumerate(header):
                self.word(value, f'CTRL_ALLOC_HEADER +{i*4:02x}')
            self.word(0xe0000440, 'DMA VLC_TABLE 1088 bytes')
            self.word(cavlc, 'CAVLC table device VA')
            self.r1([(r, v, 'fixed AVC VLC index/width/opcode') for r, v in VLC_PAIRS])
            bits = self.get(p, 'header_bit_size', path+'.params')
            size = self.get(sl, 'size', path)
            offset = self.get(sl, 'offset', path)
            skip = (bits & 0xffe0) >> 3
            if not 0 <= bits <= 0xffff or not 0 <= bits < 8*size or not 0 < size-skip < 0x10000000 or offset < 0:
                raise ValueError('invalid NAL offset/size/header_bit_size')
            for value, note in [(0xb0000001, 'SR_SETUP RBDU extraction, no start-code verify'),
                                (bits & 31, 'remaining slice header bit offset'),
                                (size-skip, 'bitstream bytes after whole-word skip'),
                                (0xa0000000 | (size-skip), 'BITSTREAM_DMA byte count'),
                                (src_va+offset+skip, 'bitstream device VA')]:
                if 'device VA' in note and not 0 <= value <= 0xffffffff:
                    raise ValueError('bitstream DMA crosses the 32-bit device address space')
                self.word(value, note)
            self.r1([(0x600c, 0x11, 'ENTDEC_FE_CONTROL'),
                     (0x6200, sps0, 'FE_SPS0'), (0x6204, pps0, 'FE_PPS0'),
                     (0x6208, pic0, 'FE_CUR_PIC0'), (0x620c, slice0, 'FE_SLICE0'),
                     (0x6210, slice1, 'FE_SLICE1'), (0x6218, first << 16, 'FE_SLICE2')])
            self.r5(0x6010, [0x11], 'ENTDEC_BE_CONTROL')
            col = self.va.get('colocated')
            if isinstance(col, list):
                col = number(col[si]) if si < len(col) else self.unknown('va.colocated', 'missing per-slice VA')
            else:
                col = self.addr('colocated')
            self.r5(0x62d8, [sps0, pps0, pic0, slice0, slice1, col, longterm], 'BE_SPS0/PPS0/PIC0/SLICE0/SLICE1/MV/REF0')
            self.r5(0xe000, self.scaling_words(pf), 'AVC scaling lists (8x8 inter/intra Y, 4x4 0,3,4,1,5,2)')
            lists = []
            for li, n in ((0, l0), (1, l1)):
                lists.append(self.list_bytes(p, li, n+1, active, path+'.params') if st != 2 and (li == 0 or st == 1) else [0xff]*32)
            if st == 1:
                self.b_chunks(dec, dpb, active, highwater, lists, l1, va_dpb, path)
            if st in (0, 1):
                self.r5(0x633c, self.pack_bytes(lists[0][:(l0//4+1)*4]), 'RefPicList0')
            if highwater:
                used = {b & 0x7f for li in lists for b in li if b != 0xff}
                addresses = []
                for di in range(highwater):
                    if di not in used:
                        addresses.extend((0xdeadbeef, 0xdeadbeef))
                        continue
                    address = va_dpb[di] if di < len(va_dpb) else None
                    if address is None:
                        addresses.extend((self.unknown(f'va.dpb[{di}].luma', 'missing reference VA'),
                                          self.unknown(f'va.dpb[{di}].chroma', 'missing reference VA')))
                    else:
                        addresses.extend((self.get(address, 'luma', f'va.dpb[{di}]'),
                                          self.get(address, 'chroma', f'va.dpb[{di}]')))
                self.r5(0x7100, addresses, 'REFERENCE_PICTURE_BASE_ADDRESSES', flags=0x08000000)
            # 62c66b0 L40086-40134: tables for explicit and implicit modes;
            # 62be348 L33194-33202: only explicit slices carry their table
            # and denominators, otherwise the record is zeroed with 5/5.
            weighted = bool(pf & 4) and st == 0 or bipred != 0 and st == 1
            explicit = bool(pf & 4) and st == 0 or bipred == 1 and st == 1
            pw = sl.get('pred_weights', {}) if explicit else {}
            if explicit and not pw:
                self.unknown(path+'.pred_weights', 'explicit weighted prediction without a table; emitted zeros')
            if weighted:
                for li in range(2 if st == 1 else 1):
                    self.r5(0x7200+0x100*li, self.weight_words(pw, li, l0 if li == 0 else l1, lists[li], path),
                            f'H264_WEIGHTED_FACTORS/OFFSET {"AB"[li]}')
            denom = 0x55
            if explicit and pw:
                denom = self.get(pw, 'chroma_log2_weight_denom', path+'.pred_weights') << 4
                denom |= self.get(pw, 'luma_log2_weight_denom', path+'.pred_weights')
            dims = (16*hmb-1) << 12 | (16*wmb-1)
            self.r5(0x7000, [dims, dims, operating_mode, dst_y, dst_c], 'display/coded size, operating mode, reconstruction Y/UV')
            self.r5(0x7018, [intra], 'INTRA_BUFFER_BASE_ADDRESS')
            self.r5(0x7024, [denom], 'H264_WEIGHTED_FACTOR_DENOMINATOR')
            self.r5(0x701c, [0x013c000c], 'MC_CACHE_CONFIGURATION')
            self.r5(0x7040, [stride & ~63], 'EXTENDED_ROW_STRIDE')
            self.r5(0x71b4, [chroma], 'alternative output pixel-format fields')
            self.word(0x60000000, 'COMPLETION')
            count = len(self.cmd)*4 - start
            if cmd_va + len(self.cmd)*4 > 0x100000000:
                raise ValueError('command buffer crosses the 32-bit device address space')
            payload = [0]*30
            payload[1] = int(si == 0) | (0x10 if self.job.get('first_picture', False) and si == 0 else 0)
            payload[2], payload[3], payload[4] = operating_mode, count, cmd_va+start
            payload[9], payload[19], payload[20] = stream_state, 1, int(not reference)
            self.packets.append({'kind': 1, 'payload': payload})
        return {'cmd': self.cmd, 'packets': self.packets,
                'unresolved': self.unresolved, 'complete': not self.unresolved,
                'recipe': 'AppleD5500 AVC progressive I/P/B, section 16.3-16.6 + vdec-test-plan round 6; no kind-4 packet'}

    def text(self, result):
        print(result['recipe'])
        print(f'complete={result["complete"]}; {len(self.cmd)} words; {len(self.packets)} packets')
        for offset, (word, note) in enumerate(zip(self.cmd, self.notes)):
            print(f'{offset*4:06x}  {word:08x}  {note}')
        for i, packet in enumerate(self.packets):
            print(f'packet[{i}] kind={packet["kind"]}, mailbox=0x81')
            for wi, word in enumerate(packet['payload']):
                print(f'  +{wi*4:02x} {word:08x}')
        for u in self.unresolved:
            print(f'[U] {u["field"]}: {u["reason"]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emit', action='store_true')
    ap.add_argument('--slots', action='store_true')
    ap.add_argument('--gen', type=pathlib.Path, metavar='IN.json', help='generate independent AVC reference vector (§16)')
    ap.add_argument('--text', action='store_true', help='pretty annotated word listing (with --gen)')
    ap.add_argument('fns', nargs='*')
    a = ap.parse_args()
    if a.gen:
        if a.emit or a.slots or a.fns:
            ap.error('--gen cannot be combined with dump views or function names')
        try:
            generator = AVCReference(json.loads(a.gen.read_text()))
            result = generator.run()
            if a.text:
                generator.text(result)
            else:
                print(json.dumps(result, indent=2))
        except (ValueError, KeyError, TypeError, OSError) as exc:
            print(f'cmdstream: {exc}', file=sys.stderr)
            return 2
        return 0
    if a.text:
        ap.error('--text requires --gen')
    lines = DEC.read_text().splitlines()
    fns, order = functions(lines)
    for name in (a.fns or order):
        if name not in fns:
            print(f'!! no {name}')
            continue
        if a.slots:
            slot_view(name, fns[name])
        else:
            emit_view(name, fns[name])


if __name__ == '__main__':
    sys.exit(main())
