#!/usr/bin/env bash
# Host/Docker only, no device access. Builds a9/aop-handoff (from aa54566a91):
# opt-in aop_handoff=sleep, reserved shutdown transcript, full crashlog dump.
# Commits only that kernel branch; workspace files stay uncommitted.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-handoff
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
uv run -q --with capstone --with unicorn python -B scripts/periph/aop/re-handoff.py > "$OUT/re-handoff.log" 2>&1
scripts/build-kernel.sh --run aop-handoff '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
parent=aa54566a91516e88eee0d79757bcb5422aa30162
if [ ! -d /src/wt/aop-handoff ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-handoff; then
        git -C /src/linux worktree add /src/wt/aop-handoff a9/aop-handoff
    else
        git -C /src/linux worktree add -b a9/aop-handoff /src/wt/aop-handoff "$parent"
    fi
fi
cd /src/wt/aop-handoff
test "$(git branch --show-current)" = a9/aop-handoff
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-handoff.h)
# Resume only when every edit is one of the two owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed = {"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-handoff.h"}
changed = set(subprocess.check_output(["git", "diff", "HEAD", "--name-only"], text=True).splitlines())
changed.update(subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines())
assert changed <= allowed, changed - allowed
PYCODE
for revision in "$base" "$parent" 3a7b3805b3 a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code "$parent" -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" ":(exclude)drivers/soc/apple/a9-aop-handoff.h"
git diff --exit-code a9/kexecfix -- drivers/pci/controller/pcie-apple-h9p.c
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
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-handoff/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-handoff/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    # The new header is covered by MAINTAINERS drivers/soc/apple/a9-aop*.
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-handoff/checkpatch.log || true
    grep -q "total: 0 errors, 0 warnings" /work/build/periph/aop-handoff/checkpatch.log
    if [ "$(git rev-parse HEAD)" != "$parent" ]; then
        git commit -q -F - <<"MSG"
soc: apple-a9-aop: handoff: prime resume watchdog, hash image before tags

The in-place resume path skipped the EPMAP handler that starts the ping
watchdog clock, so a first ping ACK later than 1 s read as a 5 s timeout.
Start it at adoption. End the resident-image fingerprint at the tag table
(0x389b8): its only direct reader loads STKG, but a tag update must never
refuse a hop.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
    else
    git commit -q -F - <<"MSG"
soc: apple-a9-aop: opt-in sleep/wake handoff, shutdown transcript, full crashlog

aop_handoff=sleep (default off, excludes aop_quiesce=1): the outgoing
kernel quiesces to AP 10 / IOP 201 sleep without halting the AOP and
certifies a versioned, consumed-once record in the reserved pool tail
page. The next kernel validates it against pool, SRAM image and wrapper
state, wakes the resident firmware with IOP 0x220 and adopts it via a
fresh HELLO or an in-place resume. No halt, reload or fallback.

Both shutdown modes append their transcript to reserved RAM; the next
probe replays it, independent of console loglevel. The RTKit crashlog
is frozen whole, its sections listed and all used bytes dumped.

Co-Authored-By: Claude Opus 5.5 <maintainer@example.invalid>
MSG
    fi
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-handoff/COMMIT
git rev-parse "$base" > /work/build/periph/aop-handoff/BASE
git rev-parse "$parent" > /work/build/periph/aop-handoff/PARENT
git diff --binary "$parent" > /work/build/periph/aop-handoff/handoff.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-handoff KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-handoff > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-handoff '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-handoff-build.py
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-handoff
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --handoff > "$OUT/package.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --handoff > "$OUT/package-test.log" 2>&1
# Retain exported images/provenance; reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-handoff '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
' > "$OUT/cleanup.log" 2>&1
