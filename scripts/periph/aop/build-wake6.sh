#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake6 (from a9/aop-wake5
# cecb33eef5): first the two a9/kexec-irqfix NVMe shared-MSI commits
# (7dcb18c411 reworded to 10 crawl boots, e3ad1b3a5c), so the kexec the warm
# reload depends on is healthy; then the round-4 AOP commit that disables
# AOP IRQ 0x10014 before the SPI0 reset and acknowledges it before RUN
# (ds-aop-spi0-ch3-round4.md section 3). Everything else is wake5.
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake6
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake6 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake5 commit first, directly in the source volume, reusing the clean
# aop-wake5 worktree (still at its own commit); a9/aop-wake5 is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=cecb33eef51a293a7e1a301b74488c8217a1ae30
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake6 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake6; then
        git -C /src/linux worktree add /src/wt/aop-wake6 a9/aop-wake6
    elif [ -d /src/wt/aop-wake5 ] &&
         [ "$(git -C /src/wt/aop-wake5 branch --show-current)" = a9/aop-wake5 ] &&
         [ "$(git -C /src/wt/aop-wake5 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake5 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake5 /src/wt/aop-wake6
        git -C /src/wt/aop-wake6 switch -c a9/aop-wake6 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake6 /src/wt/aop-wake6 "$parent"
    fi
fi
git -C /src/wt/aop-wake6 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake5)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake6 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=cecb33eef51a293a7e1a301b74488c8217a1ae30
irqfix_a=7dcb18c4119f365f2787863dfe2167aaf0349f75
irqfix_b=e3ad1b3a5c981cba722ccfee6f2352e4711b1b93
cd /src/wt/aop-wake6
test "$(git branch --show-current)" = a9/aop-wake6
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
# The NVMe crawl fix from a9/kexec-irqfix, before the AOP commit, on a clean
# tree only, each once. Commit A is reworded: 10 crawl boots, not 13.
for sha in "$irqfix_a" "$irqfix_b"; do
    subject="$(git log -1 --format=%s "$sha")"
    if git log --format=%s "$parent..HEAD" | grep -qxF "$subject"; then
        continue
    fi
    test -z "$(git status --porcelain --untracked-files=all)"
    test "$(git rev-list --count "$parent..HEAD")" = "$([ "$sha" = "$irqfix_a" ] && echo 0 || echo 1)"
    git cherry-pick -x "$sha"
    if [ "$sha" = "$irqfix_a" ]; then
        git log -1 --format=%B | sed "s/All 13 logged/All 10 logged/" | git commit -q --amend -F -
    fi
done
first="$(git log --reverse --format=%H "$parent..HEAD" | head -1)"
git log -1 --format=%B "$first" | grep -q "All 10 logged"
if git log -1 --format=%B "$first" | grep -q "13 logged"; then
    exit 1
fi
git diff --exit-code a9/kexec-irqfix -- "${nvme[@]}"
for revision in "$base" "$parent" 9ab7e00531 63eadc3456 61561869d0 f695e71125 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code "$parent" -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" \
    ":(exclude)drivers/soc/apple/a9-aop-reload.h" ":(exclude)drivers/nvme/host/pci.c" \
    ":(exclude)drivers/nvme/host/pci-internal.h"
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake6/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake6/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake6/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake6/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: clear the retained SPI0 IRQ 0x10014 on warm reload

With the channel-3 teardown, the first warm AOP reload reached SYSTEM
ready and started application endpoints 0x20..0x27, then the accel
STARTEP (0x28) timed out and the firmware panicked with SCMErr:0x3,3.
That panic comes from the SPI0 global SCM/transfer interrupt 0x10014.
Its old-session latch survives in the AOP interrupt controller, and the
fresh firmware takes it as soon as SPI lifecycle event 1 enables the
source, before event 5 runs the firmware SPI0 reset.

With the CPU halted and before the first SPI0 access, disable 0x10014
through the group-1 disable bank (+0xa00 = 0x00100000, the store at
0102c9ce). After the existing reset, teardown and clean pre-RUN window,
apply the group-1 pre-handler acknowledge (+0x980 = 0x00100000, at
0102c6ee) just before RUN; the fresh firmware re-enables the source
itself. Both banks are write-one and the firmware never reads them
back, so their readbacks, the SCM state, the channel-3 interval pair and
the +0x81c current source are logged as AOP INTC lines, not compared. A
faulted store, a failed checkpoint flush or a refused guard refuses with
its own failed-intc-* reason and leaves the AOP halted.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
test "$(git rev-list --count "$parent..HEAD")" = 3
git log -1 --format="%H %s" > /work/build/periph/aop-wake6/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake6/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake6/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake6/wake6.patch
git log --reverse --format="%H %s" "$parent..HEAD" > /work/build/periph/aop-wake6/COMMITS
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake6 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake6 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake6 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake6
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake6
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake6 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake6 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake6 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
