#!/usr/bin/env bash
# Build the A9 Mesa AP-side transport driver in the `mesa` kernel workstream.
#
# Source of truth is drivers/mesa/ in this repo: the driver, its Kconfig and
# its Makefile are copied into drivers/mesa/ of the workstream tree, and
# drivers/Kconfig / drivers/Makefile get one idempotent line each so re-running
# is safe. Only the object is built: no image, no DT, no module install.
#
# Usage:
#   scripts/sep/build-mesa-driver.sh            # build mesa-a9.o with W=1
#   scripts/sep/build-mesa-driver.sh clean      # remove the object, keep source
#   TREE=mesa-dev scripts/sep/build-mesa-driver.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TREE="${TREE:-mesa}"

if [ "${1:-}" = clean ]; then
	"$ROOT/scripts/build-kernel.sh" --run "$TREE" \
		'rm -f drivers/mesa/mesa-a9.o drivers/mesa/.mesa-a9.o.cmd'
	exit 0
fi

"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -euo pipefail

mkdir -p drivers/mesa
cp /work/drivers/mesa/mesa-a9.c drivers/mesa/mesa-a9.c
cp /work/drivers/mesa/Kconfig drivers/mesa/Kconfig
cp /work/drivers/mesa/Makefile drivers/mesa/Makefile

if ! grep -q "source \"drivers/mesa/Kconfig\"" drivers/Kconfig; then
	printf "\nsource \"drivers/mesa/Kconfig\"\n" >> drivers/Kconfig
fi

if ! grep -q "obj-y += mesa/" drivers/Makefile; then
	echo "obj-y += mesa/" >> drivers/Makefile
fi

# Keep the tree config; only the options this object needs are touched.
[ -f .config ] || cp /work/configs/hoolock-16k.config .config
scripts/config -e REGULATOR
scripts/config -m APPLE_MESA_A9
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null

# W=1 on purpose: this is a single object and the strict check is cheap.
make -j"$(nproc)" LLVM=1 ARCH=arm64 W=1 drivers/mesa/mesa-a9.o
'
