#!/usr/bin/env bash
# Reuse Mesa1's worktree and objects; commit only a9/tri53-mesa2, never push.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/sep/tri53-mesa2
docker run --rm --memory "${KERNEL_MEMORY:-4g}" --memory-swap "${KERNEL_MEMORY:-4g}" \
  -v linux-a9-src:/src -v "$ROOT:/work" -w /src/wt/tri53-mesa1 \
  -e KERNEL_JOBS="${KERNEL_JOBS:-4}" linux-a9-kbuild bash -c '
set -euo pipefail
BASE=cc0b92c36c7917d3b11ee7ad845e27b266f9fc2c
if ! git show-ref --verify --quiet refs/heads/a9/tri53-mesa2; then
  test -z "$(git status --porcelain)"
  test "$(git rev-parse HEAD)" = "$BASE"
  git switch -c a9/tri53-mesa2 "$BASE"
fi
test "$(git branch --show-current)" = a9/tri53-mesa2
if [ "$(git rev-parse HEAD)" = "$BASE" ]; then
  if [ ! -f drivers/spi/spi-apple-s8000-phase.h ]; then
    test -z "$(git status --porcelain)"
  python3 -B -I /work/scripts/sep/prepare-tri53-mesa2.py
  fi
  # Resume only our four-path preparation after an interrupted first commit.
  python3 - <<"PY"
from pathlib import Path
import subprocess
allowed = {"drivers/mesa/mesa-a9.c", "drivers/spi/spi-apple-s8000.c",
           "drivers/spi/spi-apple-s8000-phase.h",
           "arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi"}
for line in subprocess.check_output(["git", "status", "--porcelain"], text=True).splitlines():
    assert line[3:] in allowed, line
import shutil
shutil.copyfile("/work/drivers/mesa/mesa-a9.c", "drivers/mesa/mesa-a9.c")
PY
  git add drivers/mesa/mesa-a9.c drivers/spi/spi-apple-s8000.c \
    drivers/spi/spi-apple-s8000-phase.h \
    arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi
  git diff --cached --check
  git -c user.name=linux-a9 -c user.email=linux-a9@localhost \
    commit -m "arm64: apple: reproduce Mesa phase-3 SPI status timing"
fi
test "$(git rev-parse HEAD^)" = "$BASE"
test -z "$(git status --porcelain)"
python3 -B -I /work/scripts/sep/prepare-tri53-mesa2.py
if ! git diff --quiet; then
  git add drivers/mesa/mesa-a9.c drivers/spi/spi-apple-s8000.c \
    drivers/spi/spi-apple-s8000-phase.h \
    arch/arm64/boot/dts/apple/s8003-j71t-mesa-status.dtsi
  git diff --cached --check
  git -c user.name=linux-a9 -c user.email=linux-a9@localhost commit --amend --no-edit
fi
test -z "$(git status --porcelain)"
grep -qx CONFIG_DEBUG_INFO_NONE=y .config
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
# Force the touched objects to compile with warnings fatal, even on reruns.
rm -f drivers/mesa/mesa-a9.o drivers/spi/spi-apple-s8000.o
make -j"$KERNEL_JOBS" LLVM=1 ARCH=arm64 KCFLAGS=-Werror \
  drivers/mesa/mesa-a9.o drivers/spi/spi-apple-s8000.o
make -j"$KERNEL_JOBS" LLVM=1 ARCH=arm64 Image.gz dtbs
OUT=/work/build/kernel-tri53-mesa2
mkdir -p "$OUT/dtbs"
cp arch/arm64/boot/Image.gz "$OUT/"
cp arch/arm64/boot/dts/apple/*.dtb "$OUT/dtbs/"
cp .config "$OUT/config"
git log -1 --format="%H %s" >"$OUT/COMMIT"
git status --short >"$OUT/DIRTY"
git diff --binary >"$OUT/SOURCE.patch"
cat include/config/kernel.release >"$OUT/RELEASE"
git format-patch -1 --stdout > /work/build/sep/tri53-mesa2/kernel.patch
df -h /src
'
