#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check the remaining VLC index arrays and standard frame scan orders offline."""
import argparse
import hashlib
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[2]
GEN = runpy.run_path(str(Path(__file__).with_name("h264-tables.py")))


def scan(n):
    result = []
    for diagonal in range(2 * n - 1):
        rows = [r for r in range(n) if 0 <= diagonal - r < n]
        if diagonal % 2 == 0:
            rows.reverse()
        result.extend(r * n + diagonal - r for r in rows)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("driver", type=Path, help="drivers/media/platform/apple/vxd")
    args = ap.parse_args()
    source = Path(__file__).with_name("upstream") / "h264_idx.h"
    if hashlib.sha256(source.read_bytes()).hexdigest() != "fdec275fa3fe8d579d8531adbc300f5bf7a78db755b0725ae6a8bbb504fe4b99":
        raise ValueError("VLC index source hash mismatch")
    nums = GEN["array"](source.read_text(), "h264_vlc_index_data")
    if len(nums) != 38 * 3:
        raise ValueError("expected 38 index triples")
    rows = list(zip(nums[::3], nums[1::3], nums[2::3]))

    def pack(field, per_word, shift):
        return [sum(row[field] << (shift * j) for j, row in enumerate(rows[i:i + per_word]))
                for i in range(0, len(rows), per_word)]

    # img-vxe-vxd translation_api.c translation_pvdecsetup_vlc(), pvdec_int.h:
    # address: 2 x 11 bits; width: 10 x 3 bits; opcode: 16 x 2 bits.
    addresses = pack(2, 2, 11)
    low = addresses[:16] + pack(1, 10, 3) + pack(0, 16, 2)
    high = addresses[16:]
    text = (args.driver / "apple-vxd-h264-cmd.c").read_text()
    for name, expected in (("vxd_h264_vlc_addr", low), ("vxd_h264_vlc_addr_hi", high),
                           ("vxd_h264_zigzag4", scan(4)), ("vxd_h264_zigzag8", scan(8))):
        GEN["compare"](expected, GEN["array"](text, name), name)
        print(f"{name}: {len(expected)}/{len(expected)} PASS")


if __name__ == "__main__":
    main()
