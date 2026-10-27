#!/usr/bin/env bash
# Offline build/package. Reuses /src/wt/charge; no new worktree, push or device I/O.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/charge10 patches/charge10
scripts/build-kernel.sh --run charge '
set -euo pipefail
test "$(git branch --show-current)" = a9/charge10
git merge-base --is-ancestor 16a34c8432 HEAD
test -z "$(git status --porcelain --untracked-files=no)"
grep -qx "CONFIG_DEBUG_INFO_NONE=y" .config
git log -1 --format="%H %s" > /work/build/charge10/branch.txt
git format-patch --no-signature 16a34c8432..HEAD -o /work/patches/charge10
'
KERNEL_JOBS="${KERNEL_JOBS:-2}" scripts/build-kernel.sh --tree charge
python3 -B -I scripts/periph/package-charge10.py
scripts/build-kernel.sh --run charge '
set -euo pipefail
make LLVM=1 ARCH=arm64 clean >/dev/null
df -h /src | tail -1
'
python3 -B -I scripts/periph/verify-charge10.py
