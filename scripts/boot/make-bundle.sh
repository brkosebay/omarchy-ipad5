#!/usr/bin/env bash
# Package a built kernel for on-device updates (docs/boot/kernel-updates.md):
#
#   [TREE=integ] scripts/boot/make-bundle.sh [--name NAME] [--sign KEY]
#       [--ancestry FILE] [--require ARG]... [--recommend ARG]... [--remove ARG]...
#
# Reads build/kernel-$TREE/{Image.gz,dtbs,config,COMMIT,DIRTY} and
# build/boot/initramfs-omarchy.gz; writes build/boot/bundles/
# linux-a9-kernel-NAME.tar.zst (+ .sig with --sign) and linux-a9-latest.json.
# Host-only: no SSH, USB or device access; nothing is uploaded or published.
# Install on the iPad: scp the bundle, then `sudo linux-a9-update install FILE`.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
command -v zstd >/dev/null || { echo 'needs zstd (brew install zstd)' >&2; exit 1; }
exec python3 -B -I "$ROOT/scripts/boot/make-bundle.py" "$@"
