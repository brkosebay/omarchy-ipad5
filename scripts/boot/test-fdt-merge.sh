#!/usr/bin/env bash
# No device access. Default: the actual failing Round 5 pristine DTB pair.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OLD="${1:-$ROOT/build/boot/integ-entry/dtb}"
NEW="${2:-$ROOT/build/kernel-tri/dtbs/s8003-j71t.dtb}"
OLD="$(cd "$(dirname "$OLD")" && pwd)/$(basename "$OLD")"
NEW="$(cd "$(dirname "$NEW")" && pwd)/$(basename "$NEW")"
[ -s "$OLD" ] && [ -s "$NEW" ]
docker run --rm --platform linux/arm64 -v "$ROOT:/work:ro" \
    -v "$OLD:/old.dtb:ro" -v "$NEW:/new.dtb:ro" linux-a9-kbuild \
    python3 -B -I /work/scripts/boot/test-fdt-merge.py /old.dtb /new.dtb
