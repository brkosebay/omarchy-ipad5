#!/usr/bin/env bash
# Orchestrator only. Six labelled, read-only IIO captures; no boot/trigger/reset.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH=${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}
OUT=${1:-$ROOT/build/periph/aop-autorotate/poses-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
for pose in screen-up screen-down portrait landscape portrait-flipped landscape-flipped; do
  case $pose in
    screen-up) instruction='Flat, SCREEN UP' ;;
    screen-down) instruction='Flat, SCREEN DOWN (protect the glass)' ;;
    portrait) instruction='Nearly vertical, HOME button at BOTTOM' ;;
    landscape) instruction='Nearly vertical, HOME button on RIGHT' ;;
    portrait-flipped) instruction='Nearly vertical, HOME button at TOP' ;;
    landscape-flipped) instruction='Nearly vertical, HOME button on LEFT' ;;
  esac
  printf '\n%s. Hold still; press Enter to capture for 3 seconds: ' "$instruction"
  read -r answer
  test ! -e "$OUT/$pose.json" || { echo "Refusing to overwrite $OUT/$pose.json" >&2; exit 1; }
  "$SSH" "python3 - capture $pose --seconds 3" < "$ROOT/scripts/omarchy/aop-orientation.py" > "$OUT/$pose.json"
done
python3 -B "$ROOT/scripts/omarchy/aop-orientation.py" derive "$OUT" --output "$OUT/matrix.json"
cat "$OUT/matrix.json"
printf '\nInstall with: scripts/omarchy/install-aop-autorotate.sh --matrix-file %q\n' "$OUT/matrix.json"
