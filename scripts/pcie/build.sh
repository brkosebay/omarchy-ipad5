#!/usr/bin/env bash
# Build the pcie kernel with patches/pcie/pcie.config enforced, then check
# the result. Use this instead of plain `scripts/build-kernel.sh --tree pcie`:
# the tree-local .config is kept between builds and can silently lose
# options (e.g. Kconfig's syncconfig drops symbols when an older commit is
# built in the tree), which is how the round-1 kernel shipped without
# CONFIG_PCIE_APPLE_H9P.
#
# Output: build/kernel-pcie/ (as build-kernel.sh), verified config.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

"$ROOT/scripts/build-kernel.sh" --run pcie '
set -e
if [ ! -f .config ]; then
	cp /work/configs/hoolock-16k.config .config
fi
scripts/kconfig/merge_config.sh -m -O . .config /work/patches/pcie/pcie.config >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
mkdir -p /work/build/kernel-pcie
cp .config /work/build/kernel-pcie/config.merged
'
# Fail before the (long) build if the merge didn't stick
"$ROOT/scripts/pcie/check-config.sh" "$ROOT/build/kernel-pcie/config.merged"

"$ROOT/scripts/build-kernel.sh" --tree pcie

"$ROOT/scripts/pcie/check-config.sh" "$ROOT/build/kernel-pcie/config"
if [ -s "$ROOT/build/kernel-pcie/DIRTY" ]; then
	echo "build: WARNING: tree has uncommitted changes:" >&2
	cat "$ROOT/build/kernel-pcie/DIRTY" >&2
fi
echo "build: $(cat "$ROOT/build/kernel-pcie/COMMIT")"
