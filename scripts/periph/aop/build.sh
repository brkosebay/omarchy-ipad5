#!/usr/bin/env bash
# Host-only. Keep all edits/builds in branch a9/aop; never touches the iPad.
# Usage: build.sh [off|on]  (default off)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MODE="${1:-off}"
case "$MODE" in off|on) ;; *) echo 'expected off or on' >&2; exit 2 ;; esac
"$ROOT/scripts/build-kernel.sh" --run aop '
python3 -B /work/scripts/periph/aop/install-kernel.py
test -f .config || cp /work/configs/hoolock-16k.config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
'
if [ "$MODE" = on ]; then
    "$ROOT/scripts/build-kernel.sh" --run aop '
scripts/config -e DEBUG_FS -e IIO -e APPLE_PMGR_PWRSTATE -e APPLE_A9_AOP -e APPLE_A9_AOP_IIO -e APPLE_A9_AOP_ACTIVE
make LLVM=1 ARCH=arm64 olddefconfig
'
else
    "$ROOT/scripts/build-kernel.sh" --run aop '
scripts/config -d APPLE_A9_AOP -d APPLE_A9_AOP_IIO -d APPLE_A9_AOP_ACTIVE
make LLVM=1 ARCH=arm64 olddefconfig
'
fi
exec "$ROOT/scripts/build-kernel.sh" --tree aop
