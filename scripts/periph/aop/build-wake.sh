#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake (from a9/aop-handoff
# 621fe3f144): opt-in aop_spi0_reset=1 (SPI0 controller reset before a live
# reload) and aop_wake_wrapper=1 (iOS resume wrapper writes before 0x220).
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-handoff commit first, directly in the source volume.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=621fe3f144debd33b4f5dc3bc740207279892644
if [ ! -d /src/wt/aop-wake ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake; then
        git -C /src/linux worktree add /src/wt/aop-wake a9/aop-wake
    else
        git -C /src/linux worktree add -b a9/aop-wake /src/wt/aop-wake "$parent"
    fi
fi
git -C /src/wt/aop-wake merge-base --is-ancestor "$parent" HEAD
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=621fe3f144debd33b4f5dc3bc740207279892644
cd /src/wt/aop-wake
test "$(git branch --show-current)" = a9/aop-wake
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-reload.h)
# Resume only when every edit is one of the two owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-reload.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code "$parent" -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" ":(exclude)drivers/soc/apple/a9-aop-reload.h"
git diff --exit-code a9/kexecfix -- drivers/pci/controller/pcie-apple-h9p.c
# The handoff header is unchanged from the parent.
cmp drivers/soc/apple/a9-aop-handoff.h /work/scripts/periph/aop/kernel/a9-aop-handoff.h
cp /work/build/kernel-tri49a/config .config
for file in "${files[@]}"; do
    cp "/work/scripts/periph/aop/kernel/$(basename "$file")" "$file"
done
git diff --check
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-tri49a/config
for key in APPLE_A9_AOP APPLE_A9_AOP_ACTIVE APPLE_A9_AOP_IIO PSTORE PSTORE_RAM PSTORE_CONSOLE KEXEC_FILE DEBUG_INFO_NONE; do
    grep -qx "CONFIG_$key=y" .config
done
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    # The new header is covered by MAINTAINERS drivers/soc/apple/a9-aop*.
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: opt-in SPI0 pre-reset reload and resume-wrapper wake

aop_spi0_reset=1 (needs aop_run=1/2): after halting and draining a live
AOP, and before any SRAM write, replay the firmware SPI0 controller
reset (AOPSPIMaster_reset 0102f2f4: bit 8 assert, bounded polls, two
100 us delays, restore) through AP PA 0x210100000 under the AOP PMGR
guard. Every reload crashed in the SPI0 channel-fault IRQ on a retained
+0x8d0 bit 6; the load is refused unless that bit is clear after the
reset and again just before RUN. A fresh instance is never touched;
ps_spi0 is only observed.

aop_wake_wrapper=1 (needs aop_handoff=sleep): before the single 0x220,
write the AppleA7IOP status-3 resume wrapper sequence: mask 0x1111 and
unmask bits 0 then 12 on both AP views, set outbox enable. No CPU RUN,
auto-boot or NMI store. Neither option adds a fallback reload.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-wake/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake/wake.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
' > "$OUT/cleanup.log" 2>&1
