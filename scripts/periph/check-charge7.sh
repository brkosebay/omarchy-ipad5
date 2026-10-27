#!/usr/bin/env bash
# Offline only: actual policy/detector harnesses, regressions, fresh W=1 objects.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge7
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge
git merge-base --is-ancestor 2801e838fc HEAD
for f in apple_j71t_charger.c apple_tristar_power.c apple_tristar_auto.h apple_tristar_source.h; do
    cmp "drivers/power/supply/$f" "/work/scripts/periph/kernel/$f"
done
cp drivers/usb/roles/apple-s8000-usb-role.c /work/build/charge7/apple-s8000-usb-role.c
rm -f drivers/power/supply/apple_tristar_power.o \
    drivers/power/supply/apple_j71t_charger.o \
    drivers/usb/roles/apple-s8000-usb-role.o drivers/usb/gadget/udc/core.o
make -j6 LLVM=1 ARCH=arm64 W=1 \
    drivers/power/supply/apple_tristar_power.o \
    drivers/power/supply/apple_j71t_charger.o \
    drivers/usb/roles/apple-s8000-usb-role.o drivers/usb/gadget/udc/core.o \
    > /work/build/charge7/w1.log 2>&1
if grep -E "warning:|error:" /work/build/charge7/w1.log; then exit 1; fi
echo "PASS W=1: 4 freshly rebuilt objects, zero warnings/errors"
cmp .config /work/build/kernel-usb-host5b/config
git diff --check 2801e838fc
git diff --binary 2801e838fc > /work/build/charge7/kernel.patch
# This aggregate diff spans separate binding/code/DT commits. Ignore only
# the aggregate DT split warning and new-file MAINTAINERS reminder.
./scripts/checkpatch.pl --strict --no-tree \
    --ignore FILE_PATH_CHANGES,DT_SPLIT_BINDING_PATCH /work/build/charge7/kernel.patch \
    > /work/build/charge7/checkpatch.log 2>&1
echo "PASS checkpatch --strict"
'
python3 -B -I scripts/periph/test-charge7.py
python3 -B -I scripts/periph/test-charge7-tristar.py
TREE=charge scripts/periph/test-tristar-auto.sh
TREE=charge scripts/periph/test-tristar-idbus.sh
sh -n scripts/periph/charge7-watch.sh
python3 -B -I - <<'PY'
from pathlib import Path
import re
root = Path('.')
for board in ('j71s', 'j71t', 'j72s', 'j72t'):
    text = (root / f'docs/adt/{board}.txt').read_text()
    assert re.search(r'usb-input-limit-max: 0x960\b', text), board
print('PASS board ADTs: j71s/j71t/j72s/j72t all specify 2400 mA')
PY
echo 'PASS charge7 offline checks (hardware and VBUS-adaptive protection unverified)'
