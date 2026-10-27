#!/usr/bin/env bash
# Offline: run the Tristar ID-bus core (the kernel's own
# drivers/power/supply/apple_tristar_idbus.h) against a fake CBTL1610 in the
# build container, then cross-check its CRC8 against an independent Python
# reference. No device access.
#
# TREE defaults to usb-host3: the harness follows the corrected CBTL1610
# register model (TX window 0x5d..0x61, read-only RX at 0x20) and no longer
# builds against usb-host2's header (FIFO ops at 0x20/0x40/0x41).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TREE="${TREE:-usb-host3}"
scripts/build-kernel.sh --run "$TREE" '
set -euo pipefail
test "$(git branch --show-current)" = "a9/$TREE"
out=$(mktemp -d)
trap "rm -rf \"$out\"" EXIT
clang -std=gnu11 -Wall -Wextra -Werror -I drivers/power/supply \
    -o "$out/test-tristar-idbus" /work/scripts/periph/test-tristar-idbus.c
"$out/test-tristar-idbus"
python3 -B -I - "$out/test-tristar-idbus" <<"PY"
import subprocess
import sys


def crc8(data, seed=0xFF):
    # lightning-idbus.md 5.2 reference: generator 0x8c, LSB first.
    crc = seed
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8C if crc & 1 else 0)
    return crc


assert crc8(bytes.fromhex("740002")) == 0x1F  # the article capture
assert crc8(b"123456789", 0) == 0xA1          # CRC-8/MAXIM-DOW check
# Hardware 2026-10-26: reg 0x27 read 0x66 (Mac cable) and 0xd6 (camera
# adapter); the adapter frame 75 11 f0 .. d6 was retained intact once.
assert crc8(bytes.fromhex("75100c00000000")) == 0x66
assert crc8(bytes.fromhex("7511f000000000")) == 0xD6
vectors = ["740002", "740200", "72", "71", "700102",
           "75100c00000000",                   # our Mac cable
           "7511f000000000",                   # the camera adapter
           "75200000000000", "75a00000000000", "7504f100000000", "750bf000000000",
           "7300000000", "73ffffffff", "".join(f"{i:02x}" for i in range(32))]
for hexstr in vectors:
    got = subprocess.run([sys.argv[1], "--crc", hexstr], check=True,
                         capture_output=True, text=True).stdout.strip()
    want = f"{crc8(bytes.fromhex(hexstr)):02x}"
    assert got == want, (hexstr, got, want)
print(f"PASS CRC8 cross-check: {len(vectors)} vectors match the Python reference")
PY
'
