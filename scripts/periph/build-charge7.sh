#!/usr/bin/env bash
# Offline only. Build a clean a9/charge descendant of usb-host5b 2801e838fc.
# Run as a background job and poll build/charge7/build.log. No device deployment.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge7 patches/charge7
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge
git merge-base --is-ancestor 2801e838fc HEAD
test -z "$(git status --porcelain --untracked-files=no)"
if [ ! -f .config ]; then cp /work/build/kernel-usb-host5b/config .config; fi
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-usb-host5b/config
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git log -1 --format="%H %s" > /work/build/charge7/branch.txt
git format-patch --no-signature 2801e838fc..HEAD -o /work/patches/charge7
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree charge
python3 -B -I scripts/periph/package-charge7.py
scripts/build-kernel.sh --run charge '
set -euo pipefail
for board in s8000-j71s s8000-j72s s8003-j71t s8003-j72t; do
    test "$(fdtget -t u /work/build/kernel-charge/dtbs/$board.dtb \
        /soc/i2c@20a110000/pmic@3c/charger@180 apple,usb-input-limit-max-microamp)" = 2400000
done
echo "PASS four board DTB ceilings"
make LLVM=1 ARCH=arm64 clean >/dev/null
df -h /src | tail -1
'
