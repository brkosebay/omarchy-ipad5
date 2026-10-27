#!/usr/bin/env bash
# Host/Docker only; no device access or workspace commits.
# One AOP commit on a9/aop-wake7 04ff8657bb: retain every wake7 gate,
# add the firmware INTC event-3 reset and guarded read-only SCM milestones.
# Package one accel entry without loglevel or systemd.debug_shell.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake8
mkdir -p "$OUT"
# Only one kernel build on this Mac. Never stop another stream's container.
while docker ps --format "{{.Image}}" | grep -qx linux-a9-kbuild; do
    echo "waiting for the existing linux-a9-kbuild container"
    sleep 5
done
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake8 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake7 commit first, directly in the source volume, reusing the clean
# aop-wake7 worktree (still at its own commit); a9/aop-wake7 is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=04ff8657bb12f8b2a805774a7b1640df61f01bad
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake8 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake8; then
        git -C /src/linux worktree add /src/wt/aop-wake8 a9/aop-wake8
    elif [ -d /src/wt/aop-wake7 ] &&
         [ "$(git -C /src/wt/aop-wake7 branch --show-current)" = a9/aop-wake7 ] &&
         [ "$(git -C /src/wt/aop-wake7 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake7 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake7 /src/wt/aop-wake8
        git -C /src/wt/aop-wake8 switch -c a9/aop-wake8 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake8 /src/wt/aop-wake8 "$parent"
    fi
fi
git -C /src/wt/aop-wake8 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake7)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake8 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=04ff8657bb12f8b2a805774a7b1640df61f01bad
cd /src/wt/aop-wake8
test "$(git branch --show-current)" = a9/aop-wake8
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-reload.h)
nvme=(drivers/nvme/host/pci.c drivers/nvme/host/pci-internal.h)
# Resume only when every uncommitted edit is one of the two owned AOP files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-reload.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "diff", "--cached", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
# wake7 ancestry: its two irqfix cherry-picks, every earlier AOP parent, the
# kexec/ANS fixes. The NVMe files stay a9/kexec-irqfix.
for revision in "$base" "$parent" 56efc8218d e4c177ae28 cecb33eef5 9ab7e00531 63eadc3456 61561869d0 \
                f695e71125 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code a9/kexec-irqfix -- "${nvme[@]}"
git diff --exit-code "$parent" -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" \
    ":(exclude)drivers/soc/apple/a9-aop-reload.h"
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake8/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake8/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake8/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake8/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: reset retained interrupt delivery before warm RUN

Wake7 passed the SPI0 reset, channel-3 teardown and 150 ms window,
but the fresh firmware still took IRQ 0x10014 before its event-5
SPI reset and panicked SCMErr:0x3,3. The SCM value is status flags,
not an error enum, and the targeted interrupt acknowledge did not
prevent the interrupt on re-enable.

After the existing acknowledge, replay the pinned firmware INTC
event-3 sequence: wrapper +0x808 = 1/read/0/read (0102c592..5a2).
Refuse RUN after either faulted store, nonzero final readback, or
channel-3 quiet-state failure after the pulse. Keep the first reset
read and SCM observational: neither must equal one/zero. Log seven
PMGR-guarded SPI0/SCM milestones around the controller reset,
teardown, image copy, clean window and INTC reset. A refused trace
guard is terminal. Fresh-instance skips still touch no SPI0/INTC.
No sensor commands or SCM status writes; no retry or firmware patch.

The event-3 sequence is verified offline; its effect on the warm
reload failure remains for the orchestrator device test.

MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
test "$(git rev-list --count "$parent..HEAD")" = 1
git log -1 --format="%H %s" > /work/build/periph/aop-wake8/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake8/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake8/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake8/wake8.patch
git log --reverse --format="%H %s" "$parent..HEAD" > /work/build/periph/aop-wake8/COMMITS
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake8 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake8 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake8 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake8
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake8
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake8 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake8 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake8 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
