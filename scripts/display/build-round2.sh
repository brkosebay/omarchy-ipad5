#!/usr/bin/env bash
# Offline display round-2 build. Does not stage or access the device.
# TREE=display (default) builds the a9/display worktree; TREE=<integration>
# builds the matching modules for an integration kernel.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-display}"
[[ "$TREE" =~ ^[a-z0-9][a-z0-9_-]*$ ]] || exit 2
mkdir -p "$ROOT/build/kernel-$TREE"
"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -e
test -f .config || cp /work/configs/hoolock-16k.config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
scripts/kconfig/merge_config.sh -m .config /work/patches/display/display.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
grep -qx CONFIG_APPLE_ADBE_PROBE=m .config
grep -qx CONFIG_DRM_APPLE_ADBE=m .config
grep -qx CONFIG_CMA_SIZE_MBYTES=128 .config
grep -qx CONFIG_MODULE_UNLOAD=y .config
make -j"$(nproc)" LLVM=1 ARCH=arm64 W=1 Image.gz dtbs modules
out=/work/build/kernel-$TREE
mkdir -p "$out/dtbs"
cp arch/arm64/boot/Image.gz "$out/"
cp arch/arm64/boot/dts/apple/*.dtb "$out/dtbs/"
cp drivers/gpu/drm/adbe/apple_adbe_probe.ko drivers/gpu/drm/adbe/apple_adbe_drm.ko "$out/"
cp .config "$out/config"
git log -1 --format="%H %s" > "$out/COMMIT"
git status --short > "$out/DIRTY"
git diff --binary > "$out/SOURCE.patch"
cat include/config/kernel.release > "$out/RELEASE"
for ko in apple_adbe_probe apple_adbe_drm; do
	llvm-readelf -h "drivers/gpu/drm/adbe/$ko.ko" | grep -E "Class|Machine"
	modinfo -F vermagic "drivers/gpu/drm/adbe/$ko.ko" 2>/dev/null || true
done
'
