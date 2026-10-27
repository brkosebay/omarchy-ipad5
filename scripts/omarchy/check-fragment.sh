#!/usr/bin/env bash
# List every option in configs/omarchy.fragment that the final kernel .config
# does not match (merge_config.sh -m does not report these: olddefconfig drops
# options whose dependencies are missing without a word).
#
# Usage: scripts/omarchy/check-fragment.sh [built .config] [fragment]
#   default: build/kernel-omarchy/config configs/omarchy.fragment
# Exit status: 0 if everything matches, 1 otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG="${1:-$ROOT/build/kernel-omarchy/config}"
FRAGMENT="${2:-$ROOT/configs/omarchy.fragment}"

[ -f "$CONFIG" ] || { echo "missing: $CONFIG" >&2; exit 1; }

bad=0
total=0
while IFS= read -r line; do
	case "$line" in
	CONFIG_*=*)
		sym="${line%%=*}"
		want="${line#*=}"
		;;
	"# CONFIG_"*" is not set")
		sym="${line#\# }"
		sym="${sym%% *}"
		want="n"
		;;
	*) continue ;;
	esac
	total=$((total + 1))
	got="$(grep -E "^${sym}=" "$CONFIG" | head -1 | cut -d= -f2- || true)"
	[ -n "$got" ] || got="n"
	if [ "$got" != "$want" ]; then
		echo "MISMATCH $sym: want $want, got $got"
		bad=$((bad + 1))
	fi
done <"$FRAGMENT"

echo "$total options checked, $bad mismatched ($CONFIG)"
[ "$bad" -eq 0 ]
