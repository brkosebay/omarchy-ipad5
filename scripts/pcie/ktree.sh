#!/usr/bin/env bash
# Copy files between the pcie kernel worktree (/src/wt/pcie in the
# linux-a9-src Docker volume) and a host scratch dir, so they can be edited
# with normal host tools. The tree itself must stay in the volume (APFS is
# case-insensitive).
#
# Usage:
#   scripts/pcie/ktree.sh pull PATH...   # /src/wt/pcie/PATH -> $EDIT/PATH
#   scripts/pcie/ktree.sh push PATH...   # $EDIT/PATH -> /src/wt/pcie/PATH
# EDIT defaults to /tmp/pcie-edit.
set -euo pipefail

EDIT="${EDIT:-/tmp/pcie-edit}"
VOLUME="linux-a9-src"
IMAGE="linux-a9-kbuild"
MODE="${1:?pull|push}"
shift

mkdir -p "$EDIT"
case "$MODE" in
pull) SRC=/src/wt/pcie DST=/edit ;;
push) SRC=/edit DST=/src/wt/pcie ;;
*) echo "unknown mode: $MODE" >&2; exit 1 ;;
esac

docker run --rm --platform linux/arm64 -v "$VOLUME:/src" -v "$EDIT:/edit" \
	-e SRC="$SRC" -e DST="$DST" "$IMAGE" bash -c '
set -euo pipefail
for p in "$@"; do
	mkdir -p "$DST/$(dirname "$p")"
	cp -a "$SRC/$p" "$DST/$p"
	echo "$SRC/$p -> $DST/$p"
done' _ "$@"
