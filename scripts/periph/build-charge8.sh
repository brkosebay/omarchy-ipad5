#!/usr/bin/env bash
# Offline build/package. Reuses /src/wt/charge; no new worktree, push or device I/O.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge8 patches/charge8
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge8
git merge-base --is-ancestor a8aa6fea36 HEAD
test -z "$(git status --porcelain --untracked-files=no)"
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git log -1 --format="%H %s" > /work/build/charge8/branch.txt
git format-patch --no-signature a8aa6fea36..HEAD -o /work/patches/charge8
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree charge
python3 -B -I scripts/periph/package-charge8.py
scripts/build-kernel.sh --run charge '
set -euo pipefail
make LLVM=1 ARCH=arm64 clean >/dev/null
df -h /src | tail -1
'
python3 -B -I scripts/periph/verify-charge8.py
