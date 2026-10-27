#!/usr/bin/env bash
# Build the nvme-install kernel tree (/src/wt/nvme-install, branch
# a9/nvme-install = a9/pcie 946c7da17f + patches/nvme-install) with the pcie,
# Omarchy and nvme-install config fragments enforced, then check that every
# fragment option survived. Mirrors scripts/pcie/build.sh: the tree-local
# .config is re-merged on every build so options can't silently drop.
#
# Output: build/kernel-nvme-install/ (as scripts/build-kernel.sh --tree).
# The device normally boots the integration kernel instead
# (docs/nvme-install/test-plan.md, "Kernel").
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/kernel-nvme-install"
FRAGS=(patches/pcie/pcie.config configs/omarchy.fragment patches/nvme-install/nvme-install.config)

args=""
for f in "${FRAGS[@]}"; do args+=" /work/$f"; done
"$ROOT/scripts/build-kernel.sh" --run nvme-install "
set -e
[ -f .config ] || cp /work/configs/hoolock-16k.config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
scripts/kconfig/merge_config.sh -m -O . .config $args >/dev/null
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
"
"$ROOT/scripts/build-kernel.sh" --tree nvme-install

missing=0
for f in "${FRAGS[@]}"; do
	while IFS= read -r want; do
		grep -qxF "$want" "$OUT/config" || { echo "DROPPED: $want (from $f)" >&2; missing=1; }
	done < <(grep -E '^CONFIG_[A-Z0-9_]+=[ym]' "$ROOT/$f")
done
[ "$missing" = 0 ] || { echo "build: fragment options missing from $OUT/config" >&2; exit 1; }
if [ -s "$OUT/DIRTY" ]; then
	echo "build: WARNING: tree has uncommitted changes:" >&2
	cat "$OUT/DIRTY" >&2
fi
echo "build: $(cat "$OUT/COMMIT")"
