#!/usr/bin/env bash
# Remove only generated objects in verified, superseded task integrations.
# Preserve configs, sources, exported Images/DTBs, logs, rootfs and all processes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run boot '
for round in 8b 8c 8d; do
  cd /src/wt/bootr${round}-check
  test "$(git rev-parse HEAD)" = "$(cat /work/build/boot/round${round}/integration-base.txt)"
  git diff --binary > /tmp/round8-cache-before.patch
  cmp /tmp/round8-cache-before.patch /work/build/boot/round${round}/kernel.patch
  echo "Cleaning generated objects only: $PWD"
  make -s LLVM=1 ARCH=arm64 clean
  git diff --binary > /tmp/round8-cache-after.patch
  cmp /tmp/round8-cache-before.patch /tmp/round8-cache-after.patch
  test -f .config
done
df -h /src
'
