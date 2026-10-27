#!/usr/bin/env bash
# Apply the peripheral fragment in the isolated Docker worktree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/scripts/build-kernel.sh" --run periph '
if [ ! -f .config ]; then
  cp /work/configs/hoolock-16k.config .config
fi
scripts/kconfig/merge_config.sh -m .config /work/scripts/periph/kernel.config
make LLVM=1 ARCH=arm64 olddefconfig
'
