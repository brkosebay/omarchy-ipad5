#!/usr/bin/env bash
# Export the a9/pcie kernel series (everything on top of the hoolock commit it
# was branched from, 6831bc70) to
# patches/pcie/kernel/ so it survives outside the Docker volume.
# Re-apply on a fresh tree with: git am patches/pcie/kernel/*.patch
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# The main clone's `hoolock` branch moves when the pristine build refetches,
# so pin the base.
BASE="${BASE:-6831bc701a6ce059e71e5aaa9488c9195bea6927}"
exec "$ROOT/scripts/build-kernel.sh" --run pcie "BASE=$BASE"'
set -e
rm -rf /work/patches/pcie/kernel
git format-patch -q -o /work/patches/pcie/kernel "$BASE"..HEAD
git log --oneline --reverse "$BASE"..HEAD > /work/patches/pcie/kernel/SERIES
echo "base: $BASE ($(git log -1 --format=%s "$BASE"))" >> /work/patches/pcie/kernel/SERIES
ls /work/patches/pcie/kernel
'
