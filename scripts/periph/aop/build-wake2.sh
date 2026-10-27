#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake2 (from a9/aop-wake
# f695e71125): aop_spi0_reset=1 decides "firmware ran since the SoC reset"
# from auto-boot or the previous shutdown transcript (bound by +0x4020)
# instead of auto-boot alone, which the outgoing quiesce halt clears.
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake2
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake2 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake commit first, directly in the source volume.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=f695e71125f8073247991fe6f803ae6e6487f98f
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake2 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake2; then
        git -C /src/linux worktree add /src/wt/aop-wake2 a9/aop-wake2
    else
        git -C /src/linux worktree add -b a9/aop-wake2 /src/wt/aop-wake2 "$parent"
    fi
fi
git -C /src/wt/aop-wake2 merge-base --is-ancestor "$parent" HEAD
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake2 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=f695e71125f8073247991fe6f803ae6e6487f98f
cd /src/wt/aop-wake2
test "$(git branch --show-current)" = a9/aop-wake2
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-reload.h)
# Resume only when every edit is one of the two owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-reload.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake2/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake2/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake2/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake2/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: decide the SPI0 pre-reset from the previous shutdown

aop_spi0_reset=1 ran the SPI0 reset only when auto-boot read 1 before the
stop. The outgoing aop_quiesce=1 halt clears auto-boot, so a reload after
a clean quiesce looked like a DFU cold start: the reset was skipped
(reason=fresh-instance auto_boot=0) and the fresh image crashed at the
accel STARTEP on the retained channel-3 bit again.

Decide "firmware ran since the SoC reset" from the boot snapshot and the
reserved-RAM shutdown transcript of the previous kernel, parsed at probe:
auto-boot 1 (unquiesced), or a valid transcript whose SHUTDOWN begin says
hello=1 (QUIESCE complete halted=1 with the steps bit labels it), or a
SPI0-CARRY retained=1 line that an aop_spi0_reset=1 kernel now writes at
shutdown so a hop that never released the AOP passes the run on. That
DRAM can outlive a DFU while wrapper +0x4020 outbox enable cannot (set
after reset, cleared by every Linux stop), so transcript evidence counts
only while the boot snapshot has that enable bit clear. No record, a corrupt
record, a record without a run, or a stale one skips and leaves SPI0
untouched. One "SPI0 reset decision=run|skip reason=..." line precedes
the unchanged begin/checkpoint/guarded reset; SPI0 is not read to decide.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-wake2/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake2/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake2/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake2/wake2.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake2 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake2 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake2 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake2
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake2
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake2 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake2 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake2 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
