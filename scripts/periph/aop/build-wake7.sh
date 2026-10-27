#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake7 from a9/aop-wake6
# a72759f963 (which already carries the a9/kexec-irqfix NVMe commits): one
# AOP commit that gives the channel-3 idle wait the firmware's own bound
# twice over (1 ms slices, 20 ms, per-slice samples logged) and judges the
# INTC 0x10014 disable by the mask readback the firmware itself uses, with
# ten INTC words logged around both stores. Everything else is wake6.
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake7
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake7 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake6 commit first, directly in the source volume, reusing the clean
# aop-wake6 worktree (still at its own commit); a9/aop-wake6 is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=a72759f96388d68238f941d52434188db6352334
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake7 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake7; then
        git -C /src/linux worktree add /src/wt/aop-wake7 a9/aop-wake7
    elif [ -d /src/wt/aop-wake6 ] &&
         [ "$(git -C /src/wt/aop-wake6 branch --show-current)" = a9/aop-wake6 ] &&
         [ "$(git -C /src/wt/aop-wake6 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake6 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake6 /src/wt/aop-wake7
        git -C /src/wt/aop-wake7 switch -c a9/aop-wake7 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake7 /src/wt/aop-wake7 "$parent"
    fi
fi
git -C /src/wt/aop-wake7 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake6)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake7 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=a72759f96388d68238f941d52434188db6352334
cd /src/wt/aop-wake7
test "$(git branch --show-current)" = a9/aop-wake7
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
# wake6 ancestry: its two irqfix cherry-picks, every earlier AOP parent, the
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake7/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake7/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake7/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake7/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: bound the channel-3 idle wait like the firmware

At the wake6 warm reload, channel 3 had restarted a transfer after the
SPI0 reset (C3 01011445), and after the stop store its idle bit did not
return within the 1 ms the AP allowed (C3 02010444), so the reload was
refused with failed-ch3-teardown-idle. In wake5 the same poll saw idle
at once. The firmware channel disable itself (setChannelEnabled,
0102ef7c) waits until master +0x30, which lifecycle event 1 sets to
10000 us (0102e6fa) when it is zero, then panics with SCMto. Poll in
1 ms slices for up to 20 ms instead, sample C3, the SCM state and
+0x8d0 after each slice, and log every sample and the idle time.

The same run read the INTC disable bank as ffffffff before and after
the 0x10014 store. The firmware reads that bank back as the group-1
mask: its sleep command saves +0xa00..+0xa08 (0102c5ce) before masking
every source, its restore writes the saved words to +0xa00 and their
complement to +0xa80, and aop_interrupt_write_enabled reads the target
bank for the previous state (0102c9c0). The quiesce had therefore
already masked every bank-0 source. Refuse unless bit 20 reads 1 after
the store (failed-intc-disable-readback), and log ten INTC words around
both stores: the reset and current-source words, the raise and ack
banks, the three group-1 masks, the enable bank and the group-7 masks.
A current source of ffffffff, which the firmware sleep path cannot
leave behind, is labelled view=all-ones. No readable register shows a
masked source pending, so the acknowledge stays ungated.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
test "$(git rev-list --count "$parent..HEAD")" = 1
git log -1 --format="%H %s" > /work/build/periph/aop-wake7/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake7/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake7/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake7/wake7.patch
git log --reverse --format="%H %s" "$parent..HEAD" > /work/build/periph/aop-wake7/COMMITS
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake7 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake7 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake7 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake7
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake7
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake7 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake7 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake7 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
