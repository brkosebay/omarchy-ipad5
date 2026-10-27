#!/usr/bin/env bash
# Build a kernel tree with configs/omarchy.fragment merged into its .config,
# through the shared scripts/build-kernel.sh (Docker, tree /src/wt/NAME).
#
# Usage: scripts/omarchy/build-kernel.sh [--reseed] [TREE]
#   TREE      default "omarchy" (branch a9/omarchy, output build/kernel-omarchy)
#   --reseed  start the tree's .config over from configs/hoolock-16k.config
#             (debug info off, as build-kernel.sh seeds it) before merging
#
# The fragment is merged on top of whatever .config the tree already has, so
# the same command works on another workstream's tree (pcie, periph, ...) when
# the orchestrator wants Omarchy's options there; this script never reseeds a
# tree unless asked to.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RESEED=0
if [ "${1:-}" = "--reseed" ]; then
	RESEED=1
	shift
fi
TREE="${1:-omarchy}"

"$ROOT/scripts/build-kernel.sh" --run "$TREE" '
set -euo pipefail
if [ '"$RESEED"' = 1 ] || [ ! -f .config ]; then
	cp /work/configs/hoolock-16k.config .config
	scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
fi
scripts/kconfig/merge_config.sh -m .config /work/configs/omarchy.fragment >/tmp/merge.log 2>&1 ||
	{ cat /tmp/merge.log; exit 1; }
echo "merge: $(grep -c redefined /tmp/merge.log || true) base options redefined by the fragment"
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
echo "merged configs/omarchy.fragment into $(pwd)/.config"
'

"$ROOT/scripts/build-kernel.sh" --tree "$TREE"
"$ROOT/scripts/omarchy/check-fragment.sh" "$ROOT/build/kernel-$TREE/config"
