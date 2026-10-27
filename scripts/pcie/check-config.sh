#!/usr/bin/env bash
# Verify that a kernel config contains every CONFIG_ line of the pcie
# fragment verbatim ("CONFIG_X=y", "CONFIG_X=30", "# CONFIG_X is not set").
#
# Usage: scripts/pcie/check-config.sh [config]   (default build/kernel-pcie/config)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CFG="${1:-$ROOT/build/kernel-pcie/config}"
FRAG="$ROOT/patches/pcie/pcie.config"

[ -f "$CFG" ] || { echo "check-config: $CFG missing" >&2; exit 1; }
bad=0
while IFS= read -r line; do
	case "$line" in
	CONFIG_*=* | "# CONFIG_"*" is not set") ;;
	*) continue ;;
	esac
	if ! grep -qxF -- "$line" "$CFG"; then
		echo "check-config: $CFG lacks: $line" >&2
		bad=1
	fi
done <"$FRAG"
[ "$bad" = 0 ] || exit 1
echo "check-config: $CFG has all $(grep -cE '^(CONFIG_|# CONFIG_.* is not set)' "$FRAG") fragment options"
