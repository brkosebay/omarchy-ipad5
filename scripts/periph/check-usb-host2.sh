#!/usr/bin/env bash
# Offline validation of a9/usb-host2 (manual Tristar ID-bus identify).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
scripts/periph/test-tristar-idbus.sh
python3 -B -I scripts/periph/test-usb-host.py
scripts/build-kernel.sh --run usb-host2 '
set -euo pipefail
test "$(git branch --show-current)" = a9/usb-host2
test -z "$(git status --porcelain)"
git merge-base --is-ancestor 0cfbadcd4f HEAD
test "$(git rev-list --count 0cfbadcd4f..HEAD)" = 1
test "$(git diff --name-only 0cfbadcd4f HEAD | sort | tr "\n" " ")" = \
    "drivers/power/supply/apple_tristar_idbus.h drivers/power/supply/apple_tristar_power.c "
make -j4 LLVM=1 ARCH=arm64 W=1 drivers/power/supply/apple_tristar_power.o
# BAD_SIGN_OFF only flags the capitalisation of the Co-Authored-By trailer.
git format-patch --stdout -1 HEAD | ./scripts/checkpatch.pl --strict --no-tree \
    --ignore FILE_PATH_CHANGES,BAD_SIGN_OFF -
'
python3 -B -I scripts/periph/package-usb-host2.py
echo "PASS usb-host2 offline checks"
