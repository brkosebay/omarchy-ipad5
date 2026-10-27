#!/usr/bin/env bash
# Host only: install/build the ring reader on a9/aop, LLVM arm64 W=1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/build/periph/aop-ring-reader"
"$ROOT/scripts/build-kernel.sh" --run aop '
test "$(git branch --show-current)" = a9/aop
python3 -B /work/scripts/periph/aop/install-kernel.py
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE \
    -e DEBUG_FS -e IIO -e APPLE_A9_AOP -e APPLE_A9_AOP_IIO -e APPLE_A9_AOP_ACTIVE
make LLVM=1 ARCH=arm64 olddefconfig
make -j4 LLVM=1 ARCH=arm64 W=1 Image.gz dtbs > /work/build/periph/aop-ring-reader/build-w1.log 2>&1
tail -20 /work/build/periph/aop-ring-reader/build-w1.log
out=/work/build/kernel-aop
mkdir -p "$out"
rm -rf "$out/dtbs"
mkdir -p "$out/dtbs"
cp arch/arm64/boot/Image.gz "$out/"
cp arch/arm64/boot/dts/apple/*.dtb "$out/dtbs/"
cp .config "$out/config"
git log -1 --format="%H %s" > "$out/COMMIT"
git status --short > "$out/DIRTY"
git diff --binary > "$out/SOURCE.patch"
cat include/config/kernel.release > "$out/RELEASE"
'
