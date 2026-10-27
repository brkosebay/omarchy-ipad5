#!/usr/bin/env bash
# Offline build/package. Reuses /src/wt/charge; no new worktree, push or device I/O.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge9 patches/charge9
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge9
git merge-base --is-ancestor 824a8bda2c HEAD
test -z "$(git status --porcelain --untracked-files=no)"
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git log -1 --format="%H %s" > /work/build/charge9/branch.txt
git format-patch --no-signature 824a8bda2c..HEAD -o /work/patches/charge9
'
KERNEL_JOBS="${KERNEL_JOBS:-6}" scripts/build-kernel.sh --tree charge
python3 -B -I scripts/periph/package-charge9.py
scripts/build-kernel.sh --run charge '
set -euo pipefail
make LLVM=1 ARCH=arm64 clean >/dev/null
df -h /src | tail -1
'
python3 -B -I scripts/periph/verify-charge9.py
