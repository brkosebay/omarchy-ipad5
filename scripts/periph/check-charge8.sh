#!/usr/bin/env bash
# Offline only; the worktree is deliberately reused under branch a9/charge8.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge8
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge8
git merge-base --is-ancestor a8aa6fea36 HEAD
for f in apple_j71t_charger.c apple_tristar_power.c; do
    cmp "drivers/power/supply/$f" "/work/scripts/periph/kernel/$f"
done
git show a8aa6fea36:drivers/power/supply/apple_tristar_power.c > /work/build/charge8/tristar-before.c
cp drivers/usb/roles/apple-s8000-usb-role.c /work/build/charge8/apple-s8000-usb-role.c
rm -f drivers/power/supply/apple_tristar_power.o drivers/power/supply/apple_j71t_charger.o \
    drivers/usb/roles/apple-s8000-usb-role.o drivers/usb/gadget/udc/core.o
make -j6 LLVM=1 ARCH=arm64 W=1 drivers/power/supply/apple_tristar_power.o \
    drivers/power/supply/apple_j71t_charger.o drivers/usb/roles/apple-s8000-usb-role.o \
    drivers/usb/gadget/udc/core.o > /work/build/charge8/w1.log 2>&1
if grep -E "warning:|error:" /work/build/charge8/w1.log; then exit 1; fi
cmp .config /work/build/kernel-charge/config
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git diff --check a8aa6fea36
git diff --binary a8aa6fea36 > /work/build/charge8/kernel.patch
./scripts/checkpatch.pl --strict --no-tree --ignore FILE_PATH_CHANGES \
    /work/build/charge8/kernel.patch > /work/build/charge8/checkpatch.log 2>&1
out=$(mktemp -d)
trap "rm -rf \"$out\"" EXIT
for name in auto idbus; do
    clang -std=gnu11 -Wall -Wextra -Werror -I drivers/power/supply \
        -o "$out/$name" "/work/scripts/periph/test-tristar-$name.c"
    "$out/$name"
done
echo "PASS W=1: 4 fresh objects; checkpatch --strict clean; unchanged debug-info-off config"
'
python3 -B -I scripts/periph/test-charge8-tristar.py
python3 -B -I scripts/periph/test-charge8-adc.py
python3 -B -I scripts/periph/test-charge8.py
sh -n scripts/omarchy/initramfs/init scripts/periph/charge8-watch.sh
python3 -B -I scripts/periph/verify-charge8.py --sources-only
