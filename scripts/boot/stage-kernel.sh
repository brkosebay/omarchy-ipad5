#!/usr/bin/env bash
# Stage a build on the device; no reboot. See docs/boot/warm-reboot.md.
# [TREE=bootinteg] stage-kernel.sh [NAME] [--default] [--cmdline "..."]
# NAME must match linux_a9.entry= on the initial cold boot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SSH="${OMARCHY_SSH:-$ROOT/scripts/omarchy/ssh.sh}"
TREE="${TREE:-bootinteg}"
K="$ROOT/build/kernel-$TREE"
INITRD="${INITRD:-$ROOT/build/boot/initramfs-omarchy.gz}"
NAME="" DEFAULT="" CMDLINE=""
while [ $# -gt 0 ]; do
    case "$1" in
    --default) DEFAULT=--default ;;
    --cmdline) CMDLINE=${2:?missing cmdline}; shift ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) [ -z "$NAME" ] || { echo 'only one entry name allowed' >&2; exit 2; }; NAME=$1 ;;
    esac
    shift
done
NAME="${NAME:-$TREE-$(cut -c1-10 "$K/COMMIT")}"
compat=$("$SSH" "tr '\\0' '\\n' </proc/device-tree/compatible | head -1")
case "$compat" in
apple,j71t) dtb=s8003-j71t.dtb ;;
apple,j72t) dtb=s8003-j72t.dtb ;;
apple,j71s) dtb=s8000-j71s.dtb ;;
apple,j72s) dtb=s8000-j72s.dtb ;;
*) echo "unexpected board '$compat'" >&2; exit 1 ;;
esac
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp "$K/Image.gz" "$stage/Image.gz"
cp "$K/dtbs/${DTB:-$dtb}" "$stage/dtb"
cp "$K/COMMIT" "$stage/COMMIT"
cp "$INITRD" "$stage/initramfs.gz"
[ -z "$CMDLINE" ] || printf '%s\n' "$CMDLINE" >"$stage/cmdline"
"$ROOT/scripts/boot/upload-entry.sh" "$NAME" "$stage" ${DEFAULT:+"$DEFAULT"}
