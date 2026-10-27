#!/usr/bin/env bash
# Copy files between the nvme-install kernel worktree (/src/wt/nvme-install,
# branch a9/nvme-install, in the linux-a9-src Docker volume) and a host
# scratch dir, so they can be edited with host tools. The tree stays in the
# volume (APFS is case-insensitive). Same idea as scripts/pcie/ktree.sh.
#
# Usage:
#   scripts/nvme-install/ktree.sh pull PATH...   # tree/PATH -> $EDIT/PATH
#   scripts/nvme-install/ktree.sh push PATH...   # $EDIT/PATH -> tree/PATH
#   scripts/nvme-install/ktree.sh git ARGS...    # git ARGS in the tree
# EDIT defaults to /tmp/nvi-edit.
set -euo pipefail

EDIT="${EDIT:-/tmp/nvi-edit}"
TREE=/src/wt/nvme-install
MODE="${1:?pull|push|git}"
shift

mkdir -p "$EDIT"
case "$MODE" in
pull) SRC=$TREE DST=/edit ;;
push) SRC=/edit DST=$TREE ;;
git)
	exec docker run --rm --platform linux/arm64 -v linux-a9-src:/src -v "$EDIT:/edit" \
		linux-a9-kbuild bash -c 'git config --global --add safe.directory "*"
git config --global user.name "linux-a9"; git config --global user.email "linux-a9@localhost"
cd '"$TREE"' && git "$@"' _ "$@"
	;;
*) echo "unknown mode: $MODE" >&2; exit 1 ;;
esac

docker run --rm --platform linux/arm64 -v linux-a9-src:/src -v "$EDIT:/edit" \
	-e SRC="$SRC" -e DST="$DST" linux-a9-kbuild bash -c '
set -euo pipefail
for p in "$@"; do
	mkdir -p "$DST/$(dirname "$p")"
	cp -a "$SRC/$p" "$DST/$p"
	echo "$SRC/$p -> $DST/$p"
done' _ "$@"
