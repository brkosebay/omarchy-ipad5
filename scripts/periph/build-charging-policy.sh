#!/usr/bin/env bash
# Full arm64 LLVM/W=1 build on the owning branch, then export normal artifacts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JOBS="${KERNEL_JOBS:-2}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || exit 2
mkdir -p "$ROOT/build/periph/charging-policy"
# Passing a validated integer is safe; build-kernel.sh otherwise owns Docker setup.
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test "$(git branch --show-current)" = a9/periph
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$(cat /work/patches/periph/charging-policy/HEAD)"
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
make LLVM=1 ARCH=arm64 W=1 -j'"$JOBS"' Image.gz dtbs
out=/work/build/kernel-periph
mkdir -p "$out/dtbs"
cp arch/arm64/boot/Image.gz "$out/"
cp arch/arm64/boot/dts/apple/*.dtb "$out/dtbs/"
cp .config "$out/config"
git log -1 --format="%H %s" > "$out/COMMIT"
git status --short > "$out/DIRTY"
git diff --binary > "$out/SOURCE.patch"
cat include/config/kernel.release > "$out/RELEASE"
python3 -B -I /work/scripts/periph/verify-kernel.py
printf "done: %s dtbs; W=1 arm64 LLVM=1\n" "$(find "$out/dtbs" -name "*.dtb" | wc -l)"
'
