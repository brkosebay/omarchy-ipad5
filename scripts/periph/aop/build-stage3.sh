#!/usr/bin/env bash
# Host/Docker only. Commit only the dedicated a9/aop-stage3 kernel branch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT=build/periph/aop-stage3
mkdir -p "$OUT"
python3 -B scripts/periph/aop/test-offline.py > "$OUT/offline.log" 2>&1
scripts/build-kernel.sh --run tri49a '
set -euo pipefail
base=c34db1b80e859565c6cdb180b0b177c396b439d1
if [ ! -d /src/wt/aop-stage3 ]; then
    if git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-stage3; then
        git -C /src/linux worktree add /src/wt/aop-stage3 a9/aop-stage3
    else
        git -C /src/linux worktree add -b a9/aop-stage3 /src/wt/aop-stage3 "$base"
    fi
fi
cd /src/wt/aop-stage3
test "$(git branch --show-current)" = a9/aop-stage3
# Resume an interrupted prepare only when every edit is one of our owned files.
python3 -B - <<"PYCODE"
import subprocess
allowed={"drivers/soc/apple/apple-a9-aop-active.c", "drivers/soc/apple/a9-aop-system.h", "drivers/soc/apple/a9-aop-protocol.h"}
changed=set(subprocess.check_output(["git","diff","HEAD","--name-only"],text=True).splitlines())
changed.update(subprocess.check_output(["git","ls-files","--others","--exclude-standard"],text=True).splitlines())
assert changed <= allowed, changed-allowed
PYCODE
git merge-base --is-ancestor "$base" HEAD
# The warm experiment must retain the r5 calibration-state report gate.
git merge-base --is-ancestor 3a7b3805b31c2cba71deeff6b5eff6adf7b245d0 HEAD
git diff --exit-code 3a7b3805b3 -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" ":(exclude)drivers/soc/apple/a9-aop-system.h"
files=(drivers/soc/apple/apple-a9-aop-active.c drivers/soc/apple/a9-aop-system.h drivers/soc/apple/a9-aop-protocol.h)
git diff --exit-code "$base" -- . ":(exclude)drivers/soc/apple/apple-a9-aop-active.c" ":(exclude)drivers/soc/apple/a9-aop-system.h" ":(exclude)drivers/soc/apple/a9-aop-protocol.h"
# tri49a contains the kexecfix changes as cherry-picks a8b7681bdb/a003e82cad.
for revision in a8b7681bdb a003e82cad 76caa84ff2 8bb2e70bce; do
    git merge-base --is-ancestor "$revision" HEAD
done
git diff --exit-code a9/kexecfix -- drivers/pci/controller/pcie-apple-h9p.c
cp /work/build/kernel-tri49a/config .config
cp /work/scripts/periph/aop/kernel/apple-a9-aop-active.c drivers/soc/apple/
cp /work/scripts/periph/aop/kernel/a9-aop-system.h drivers/soc/apple/
cp /work/scripts/periph/aop/kernel/a9-aop-protocol.h drivers/soc/apple/
git diff --check
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-tri49a/config
for key in APPLE_A9_AOP APPLE_A9_AOP_ACTIVE APPLE_A9_AOP_IIO PSTORE PSTORE_RAM PSTORE_CONSOLE KEXEC_FILE DEBUG_INFO_NONE; do
    grep -qx "CONFIG_$key=y" .config
done
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o > /work/build/periph/aop-stage3/w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-stage3/w1.log; then
    exit 1
fi
if ! git diff --quiet HEAD -- "${files[@]}" || [ -n "$(git ls-files --others --exclude-standard)" ]; then
    git add -- "${files[@]}"
    # New header is already covered by MAINTAINERS drivers/soc/apple/a9-aop*.
    git diff --cached | scripts/checkpatch.pl --no-tree --ignore FILE_PATH_CHANGES - > /work/build/periph/aop-stage3/checkpatch.log
    git commit -m "soc: apple-a9-aop: quiesce RTKit before warm reload"
fi
test -z "$(git status --porcelain --untracked-files=all)"
git log -1 --format="%H %s" > /work/build/periph/aop-stage3/COMMIT
git rev-parse "$base" > /work/build/periph/aop-stage3/BASE
git diff --binary "$base" > /work/build/periph/aop-stage3/stage3.patch
' > "$OUT/prepare.log" 2>&1
KERNEL_LOCALVERSION=-aop-stage3 KERNEL_JOBS=${KERNEL_JOBS:-4} \
    scripts/build-kernel.sh --tree aop-stage3 > "$OUT/build.log" 2>&1
scripts/build-kernel.sh --run aop-stage3 '
set -euo pipefail
python3 -B -I /work/scripts/periph/aop/check-stage3.py
python3 -B -I /work/scripts/periph/aop/tests/cold-pstore.py /src/wt/aop-stage3
' > "$OUT/check-build.log" 2>&1
python3 -B scripts/periph/aop/package-stage3.py --warm > "$OUT/package-warm.log" 2>&1
python3 -B scripts/periph/aop/tests/stage3-package.py --warm > "$OUT/package-warm-test.log" 2>&1
# Retain exported images/provenance, reclaim only this branch's build products.
scripts/build-kernel.sh --run aop-stage3 '
set -euo pipefail
du -sh .
make LLVM=1 ARCH=arm64 clean >/dev/null
test -z "$(git status --porcelain --untracked-files=all)"
du -sh .
' > "$OUT/cleanup.log" 2>&1
