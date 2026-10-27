#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake3 (from a9/aop-wake2
# 61561869d0): after the SPI0 reset, aop_spi0_reset=1 also tears channel 3
# down (ds-aop-spi0-ch3-round3.md section 5) and samples +0x8d0 bit 6 for
# >= 150 ms after the image copy before it releases RUN once.
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake3
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake3 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake2 commit first, directly in the source volume. /src is nearly
# full, so a clean aop-wake2 worktree (still at its own commit) is moved and
# reused rather than checking out a second ~2 GB tree; a9/aop-wake2 itself
# is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=61561869d00295658dffac65974c95b5c08e5428
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake3 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake3; then
        git -C /src/linux worktree add /src/wt/aop-wake3 a9/aop-wake3
    elif [ -d /src/wt/aop-wake2 ] &&
         [ "$(git -C /src/wt/aop-wake2 branch --show-current)" = a9/aop-wake2 ] &&
         [ "$(git -C /src/wt/aop-wake2 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake2 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake2 /src/wt/aop-wake3
        git -C /src/wt/aop-wake3 switch -c a9/aop-wake3 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake3 /src/wt/aop-wake3 "$parent"
    fi
fi
git -C /src/wt/aop-wake3 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake2)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake3 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=61561869d00295658dffac65974c95b5c08e5428
cd /src/wt/aop-wake3
test "$(git branch --show-current)" = a9/aop-wake3
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-reload.h)
# Resume only when every edit is one of the two owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-reload.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" f695e71125 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake3/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake3/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake3/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake3/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: tear SPI0 channel 3 down before a warm reload

aop_spi0_reset=1 replays the firmware SPI0 controller reset with the AOP
CPU halted. On the device it cleared +0x8d0 bit 6 (channel 3 fault) but
left channel 3 idle yet enabled, async-armed and with its class-0 IRQ on
(C3 0x00010447), so a level source re-latched bit 6 about 105 ms later,
still before RUN, and the reload was refused.

After a reset that clears bit 6, and before the image copy, replay the
firmware channel teardown (AOPSPIMaster_teardownChannel 0102e4f8) and
the fault handler acknowledge (0102f20a) on channel 3, inside the same
AOP PMGR guard and after a flushed begin line: C3 &= ~0x411, poll idle
bit 1 (1 ms bound), C3 &= ~0x450, C3 &= ~0x510, descriptor = 0, C3C |=
0x20. Every store is a read-modify-write except the descriptor, which
the firmware zeroes. Then require C3 bits 0/6/8 clear and bit 1 set, a
zero descriptor, C3C bit 5 set and +0x8d0 bit 6 clear; any mismatch, an
idle timeout, a refused guard or a failed flush refuses with its own
failed-ch3-teardown-* reason and nothing is loaded.

After the copy, sample +0x8d0 31 times at least 5 ms apart, each inside
the guard, and require >= 150 ms in total. A set bit 6 logs its sample,
time and the channel-3 registers and refuses RUN as
failed-ch3-reasserted, replacing failed-spi0-reasserted; a refused guard
or a short window also refuses. On pass RUN is released once, with no
retry. Log lines use the "SPI0 CH3TD" prefix; debugfs state gains a
spi0_ch3 line.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-wake3/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake3/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake3/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake3/wake3.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake3 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake3 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake3 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake3
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake3
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake3 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake3 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake3 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
