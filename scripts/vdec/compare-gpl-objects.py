#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compare full driver objects and extract the ELF symbol's exact table bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct


def symbol_bytes(path, name):
    raw = path.read_bytes()
    if raw[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected ELF64 little endian")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    entsize, count = struct.unpack_from("<HH", raw, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * entsize) for i in range(count)]
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[sec[6]]
        for offset in range(sec[4], sec[4] + sec[5], sec[9]):
            st_name, _, _, index, value, size = struct.unpack_from("<IBBHQQ", raw, offset)
            start = strings[4] + st_name
            if raw[start:raw.index(b"\0", start)].decode() == name:
                section = sections[index]
                return raw[section[4] + value:section[4] + value + size]
    raise ValueError(f"symbol not found: {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path, help="contains before/ and after/")
    args = ap.parse_args()
    hashes = {}
    before = sorted(p.name for p in (args.directory / "before").glob("*.o"))
    after = sorted(p.name for p in (args.directory / "after").glob("*.o"))
    if before != after or len(before) != 10:
        raise ValueError("expected the same ten driver objects")
    for name in before:
        a = (args.directory / "before" / name).read_bytes()
        b = (args.directory / "after" / name).read_bytes()
        if a != b:
            raise ValueError(f"full object differs: {name}")
        hashes[name] = hashlib.sha256(a).hexdigest()
    table = symbol_bytes(args.directory / "after/apple-vxd-h264.o", "apple_vxd_h264_vlc_table")
    generator = runpy.run_path(str(Path(__file__).with_name("h264-tables.py")))
    expected = struct.pack("<544H", *generator["packed"]())
    if table != expected:
        raise ValueError("compiled table differs from packed GPL source")
    print(json.dumps({"full_object_equality": True, "objects": hashes,
                      "table_bytes": len(table), "table_sha256": hashlib.sha256(table).hexdigest()}, indent=2))


if __name__ == "__main__":
    main()
