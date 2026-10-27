#!/usr/bin/env python3
"""Export this iPad's saved /chosen chip ID to a private RAM firmware file.

No device access, synthetic identity or storage write. Preserve the eight
little-endian ADT bytes; keep the result under ignored build/, out of git.
NONC is generated afresh by the kernel and never copied from the saved dump.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def extract(text):
    chosen = False
    found = []
    for line in text.splitlines():
        if '+-o ' in line:
            chosen = bool(re.search(r'\+-o chosen\s+<class ', line))
        if chosen:
            match = re.search(r'"unique-chip-id" = <([0-9a-fA-F]*)>', line)
            if match:
                found.append(bytes.fromhex(match[1]))
    if len(found) != 1 or len(found[0]) != 8 or not any(found[0]):
        raise ValueError('need exactly one nonzero eight-byte /chosen unique-chip-id')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, default=ROOT / 'build/firmware/private/IODeviceTree.txt')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/periph/aop-force-cold/boot-ecid.bin')
    args = parser.parse_args()
    output = args.output.resolve()
    output.relative_to(ROOT / 'build')
    data = extract(args.dump.read_text(errors='replace'))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.touch(mode=0o600)
    output.chmod(0o600)
    output.write_bytes(data)
    print(f'{output}: eight saved ADT bytes; identity value withheld; no nonce reused')


if __name__ == '__main__':
    main()
