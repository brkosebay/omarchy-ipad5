#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake5 (from a9/aop-wake4
# 9ab7e00531): the channel-3 teardown treats C3C bit 5 as the self-clearing
# pulse the 19:50 device run showed (0x00200029 written, 0x00200009 read,
# +0x8d0 bit 6 cleared) and judges the acknowledge by +0x8d0 bit 6 alone.
# Everything else, including the sequential AFK quiesce and every other
# readback gate, is unchanged. Commits only that kernel branch; workspace
# files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake5
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake5 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake4 commit first, directly in the source volume, reusing the clean
# aop-wake4 worktree (still at its own commit); a9/aop-wake4 is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=9ab7e00531a75e9f31f500a1ec51ac1ee349e83d
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake5 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake5; then
        git -C /src/linux worktree add /src/wt/aop-wake5 a9/aop-wake5
    elif [ -d /src/wt/aop-wake4 ] &&
         [ "$(git -C /src/wt/aop-wake4 branch --show-current)" = a9/aop-wake4 ] &&
         [ "$(git -C /src/wt/aop-wake4 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake4 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake4 /src/wt/aop-wake5
        git -C /src/wt/aop-wake5 switch -c a9/aop-wake5 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake5 /src/wt/aop-wake5 "$parent"
    fi
fi
git -C /src/wt/aop-wake5 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake4)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake5 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=9ab7e00531a75e9f31f500a1ec51ac1ee349e83d
cd /src/wt/aop-wake5
test "$(git branch --show-current)" = a9/aop-wake5
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-reload.h)
# Resume only when every edit is one of the two owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-reload.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" 63eadc3456 61561869d0 f695e71125 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake5/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake5/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake5/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake5/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: judge the channel-3 acknowledge by +0x8d0 bit 6

The warm-reload channel-3 teardown ORs C3C (+0x28c) bit 5, the fault
handler acknowledge, and then required that bit to read back set. On
the device the bit is a self-clearing pulse: 0x00200029 was written,
0x00200009 read back, and +0x8d0 bit 6 went from 1 to 0. Every other
readback held, yet the reload was refused as failed-ch3-teardown-ack.

Drop the C3C readback gate. The acknowledge is judged by +0x8d0 bit 6,
which the teardown already requires clear, followed by the unchanged
31-sample pre-RUN window of at least 150 ms. The C3C readback is still
logged, now as c3c_bit5= on the check line, and the written word must
carry bit 5. The run, class-0, class-1, idle and descriptor gates and
every other refusal stay as they were.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-wake5/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake5/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake5/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake5/wake5.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake5 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake5 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake5 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake5
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake5
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake5 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake5 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake5 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
