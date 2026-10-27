#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-wake4 (from a9/aop-wake3
# 63eadc3456): the aop_quiesce=1 shutdown stops one AFK endpoint at a time
# (iOS AppleSPU_stopEndpoint order) instead of a 17-stop burst that lost
# every ACK past the 8-word outbox at 17:35. The channel-3 teardown is
# unchanged. Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-wake4
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-spi0-reload.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/check-warm-shutdown.py --self-test >> "$OUT/offline.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake4 --plan-only >> "$OUT/offline.log" 2>&1
# build-kernel.sh would start a missing tree at hoolock: create it from the
# a9/aop-wake3 commit first, directly in the source volume. /src is nearly
# full, so the clean aop-wake3 worktree (still at its own commit) is moved
# and reused; a9/aop-wake3 itself is untouched.
docker run --rm --platform linux/arm64 -v linux-a9-src:/src linux-a9-kbuild bash -c '
set -euo pipefail
git config --global --add safe.directory "*"
parent=63eadc3456ab9a66dfee1693574a7d9e0b708eec
df -h /src | tail -1
if [ ! -d /src/wt/aop-wake4 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-wake4; then
        git -C /src/linux worktree add /src/wt/aop-wake4 a9/aop-wake4
    elif [ -d /src/wt/aop-wake3 ] &&
         [ "$(git -C /src/wt/aop-wake3 branch --show-current)" = a9/aop-wake3 ] &&
         [ "$(git -C /src/wt/aop-wake3 rev-parse HEAD)" = "$parent" ] &&
         [ -z "$(git -C /src/wt/aop-wake3 status --porcelain --untracked-files=all)" ]; then
        git -C /src/linux worktree move /src/wt/aop-wake3 /src/wt/aop-wake4
        git -C /src/wt/aop-wake4 switch -c a9/aop-wake4 "$parent"
    else
        git -C /src/linux worktree add -b a9/aop-wake4 /src/wt/aop-wake4 "$parent"
    fi
fi
git -C /src/wt/aop-wake4 merge-base --is-ancestor "$parent" HEAD
test "$(git -C /src/linux rev-parse a9/aop-wake3)" = "$parent"
df -h /src | tail -1
' > "$OUT/worktree.log" 2>&1
scripts/build-kernel.sh --run aop-wake4 '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=63eadc3456ab9a66dfee1693574a7d9e0b708eec
cd /src/wt/aop-wake4
test "$(git branch --show-current)" = a9/aop-wake4
file=drivers/soc/apple/apple-a9-aop-active.c
# Resume only when every edit is the one owned file.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" 61561869d0 f695e71125 621fe3f144 aa54566a91 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code "$parent" -- . ":(exclude)$file"
git diff --exit-code a9/kexecfix -- drivers/pci/controller/pcie-apple-h9p.c
# The handoff and reload headers (channel-3 teardown) are unchanged from the parent.
cmp drivers/soc/apple/a9-aop-handoff.h /work/scripts/periph/aop/kernel/a9-aop-handoff.h
cmp drivers/soc/apple/a9-aop-reload.h /work/scripts/periph/aop/kernel/a9-aop-reload.h
cp /work/build/kernel-tri49a/config .config
cp "/work/scripts/periph/aop/kernel/$(basename "$file")" "$file"
git diff --check
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-tri49a/config
for key in APPLE_A9_AOP APPLE_A9_AOP_ACTIVE APPLE_A9_AOP_IIO PSTORE PSTORE_RAM PSTORE_CONSOLE KEXEC_FILE DEBUG_INFO_NONE; do
    grep -qx "CONFIG_$key=y" .config
done
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-wake4/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-wake4/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "$file" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "$file"
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-wake4/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-wake4/checkpatch.log
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: stop AFK endpoints one at a time on quiesce

The aop_quiesce=1 shutdown sent AFK 0xa4 to all 17 application
endpoints back to back, then waited 1 s for every 0x87. The AOP->AP
outbox holds eight words and nothing drained it during the burst. With
the console at loglevel 8 each send printed two lines (about 3 ms each on
this framebuffer console), so the firmware answered faster than the
burst went out: only the first eight ACKs (0x20..0x27) arrived,
0x28..0x32 stayed stopping, the quiesce failed and the next kernel
correctly refused the warm reload.

Follow iOS AppleSPU_stopEndpoint instead: drain RX, send one 0xa4, and
service RX inline until the ACK of that endpoint, bounded by 2 s per
endpoint and 10 s in total. The first missing ACK ends the sequence
with no further stop and no resend, and the transcript records the
wait, the words received and the endpoints left unsent. Every afk-ack
line now carries ms= and rx=. The endpoint-disable sends drain RX before
each word, the AP and IOP power waits get the same 2 s bound, and the
default (non-quiesce) shutdown stops its endpoints the same way.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-wake4/COMMIT
git rev-parse "$base" > /work/build/periph/aop-wake4/BASE
git rev-parse "$parent" > /work/build/periph/aop-wake4/PARENT
git diff --binary "$parent" > /work/build/periph/aop-wake4/wake4.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-wake4 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-wake4 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-wake4 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py --wake4
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-wake4
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --wake4 > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --wake4 > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-wake4 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
df -h /src | tail -1
' > "$OUT/cleanup.log" 2>&1
