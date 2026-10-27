#!/usr/bin/env bash
# Offline only. Check the actual sources on a9/charge10 in the reused charge tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge10
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge10
git merge-base --is-ancestor 16a34c8432 HEAD
for f in apple_j71t_charger.c apple_tristar_power.c; do
    cmp "drivers/power/supply/$f" "/work/scripts/periph/kernel/$f"
done
git apply --reverse --check /work/scripts/periph/charge9-support.patch
git show a8aa6fea36:drivers/power/supply/apple_tristar_power.c > /work/build/charge10/tristar-legacy.c
git show 16a34c8432:drivers/power/supply/apple_tristar_power.c > /work/build/charge10/tristar-before.c
git show 824a8bda2c:drivers/power/supply/apple_tristar_power.c > /work/build/charge10/tristar-notifier-before.c
cp drivers/usb/roles/apple-s8000-usb-role.c /work/build/charge10/apple-s8000-usb-role.c
cp drivers/usb/gadget/udc/core.c /work/build/charge10/udc-core.c
cp include/linux/usb/gadget.h /work/build/charge10/gadget.h
cp drivers/power/supply/bq27xxx_battery.c /work/build/charge10/bq27xxx_battery.c
rm -f drivers/power/supply/apple_tristar_power.o drivers/power/supply/apple_j71t_charger.o \
    drivers/power/supply/bq27xxx_battery.o drivers/usb/roles/apple-s8000-usb-role.o \
    drivers/usb/gadget/udc/core.o
make -j2 LLVM=1 ARCH=arm64 W=1 drivers/power/supply/apple_tristar_power.o \
    drivers/power/supply/apple_j71t_charger.o drivers/power/supply/bq27xxx_battery.o \
    drivers/usb/roles/apple-s8000-usb-role.o drivers/usb/gadget/udc/core.o \
    > /work/build/charge10/w1.log 2>&1
if grep -E "warning:|error:" /work/build/charge10/w1.log; then exit 1; fi
cmp .config /work/build/kernel-charge/config
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git diff --check 16a34c8432
git diff --binary 16a34c8432 > /work/build/charge10/kernel.patch
./scripts/checkpatch.pl --strict --no-tree --ignore FILE_PATH_CHANGES \
    /work/build/charge10/kernel.patch > /work/build/charge10/checkpatch.log 2>&1
out=$(mktemp -d)
trap "rm -rf \"$out\"" EXIT
for name in auto idbus; do
    clang -std=gnu11 -Wall -Wextra -Werror -I drivers/power/supply \
        -o "$out/$name" "/work/scripts/periph/test-tristar-$name.c"
    "$out/$name"
done
echo "PASS W=1: 5 fresh objects; strict checkpatch clean; unchanged debug-info-off config"
'
python3 -B -I scripts/periph/charge10-trace-fixture.py
python3 -B -I scripts/periph/test-charge10-tristar.py
python3 -B -I scripts/periph/test-charge10-adc.py
python3 -B -I scripts/periph/test-charge10-gauge.py
python3 -B -I scripts/periph/test-charge10.py
sh -n scripts/periph/charge10-watch.sh
scripts/build-kernel.sh --run charge 'python3 -B -I /work/scripts/periph/test-charge10-watch.py'
python3 -B -I scripts/periph/verify-charge10.py --sources-only
