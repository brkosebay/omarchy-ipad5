#!/usr/bin/env bash
# No device/SSH calls. Tests production scripts using fixtures and mock syscalls.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker run --rm --platform linux/arm64 -v "$ROOT:/work:ro" linux-a9-kbuild bash -c '
set -euo pipefail
command -v cpio >/dev/null
python3 -B -I /work/scripts/boot/test-offline.py
for f in /work/scripts/boot/*.sh /work/scripts/boot/device/linux-a9-kexec /work/scripts/boot/device/phoenix-init /work/scripts/boot/device/phoenix-storage /work/scripts/boot/device/linux-a9-pstore; do bash -n "$f"; done
'
