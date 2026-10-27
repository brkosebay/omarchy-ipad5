#!/usr/bin/env bash
# Offline validation of a9/usb-host3 (CBTL1610 ID-bus TX window, reorient RX,
# link recovery). Needs the build outputs of build-usb-host3.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TREE=usb-host3 scripts/periph/test-tristar-idbus.sh
python3 -B -I scripts/periph/test-usb-host.py
scripts/build-kernel.sh --run usb-host3 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host3
test -z "$(git status --porcelain)"
git merge-base --is-ancestor 9977e33ee6 HEAD
test "$(git rev-list --count 9977e33ee6..HEAD)" = 1
test "$(git diff --name-only 9977e33ee6 HEAD | sort | tr "\n" " ")" = \
    "drivers/power/supply/apple_tristar_idbus.h drivers/power/supply/apple_tristar_power.c "
# The other generation'"'"'s registers and the RX window are never written.
! git grep -nE "REG_(FIFO|TX_LEN|RX_LEN)|fifo_(read|write)" -- drivers/power/supply/apple_tristar_idbus.h drivers/power/supply/apple_tristar_power.c
make -j4 LLVM=1 ARCH=arm64 W=1 drivers/power/supply/apple_tristar_power.o
# BAD_SIGN_OFF only flags the capitalisation of the Co-Authored-By trailer.
git format-patch --stdout -1 HEAD | ./scripts/checkpatch.pl --strict --no-tree \
    --ignore FILE_PATH_CHANGES,BAD_SIGN_OFF -
make LLVM=1 ARCH=arm64 clean >/dev/null
'
python3 -B -I scripts/periph/package-usb-host3.py
echo "PASS usb-host3 offline checks"
