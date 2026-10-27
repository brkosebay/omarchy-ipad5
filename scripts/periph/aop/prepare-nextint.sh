#!/usr/bin/env bash
# Host only. Dormant integration branch, NOT production/default promotion.
# Retain nextint's SEP/suspend/USB/audio changes and cherry-pick kexecfix.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/build/periph/aop-autorotate"
"$ROOT/scripts/build-kernel.sh" --run aop-stage3 '
set -euo pipefail
base=db6d504a412b2c63d565a69a51fb16378b9d74c6
stage3=aa54566a91516e88eee0d79757bcb5422aa30162
tree=/src/wt/aop-nextint-prep
if ! git -C /src/linux show-ref --verify --quiet refs/heads/a9/aop-nextint-prep; then
    git -C /src/linux worktree add -b a9/aop-nextint-prep "$tree" "$base"
fi
test -d "$tree"
cd "$tree"
test "$(git branch --show-current)" = a9/aop-nextint-prep
test -z "$(git status --porcelain)"
for revision in 6d449cccb367476701872e683021be769c1f5dca a412bc8f04aeb9ac04796064d127db59811510fc; do
    picked=$(git log --format=%B "$base..HEAD")
    if ! git merge-base --is-ancestor "$revision" HEAD && ! grep -Fq "$revision" <<< "$picked"; then
        if ! git cherry-pick -x "$revision"; then
            test "$revision" = a412bc8f04aeb9ac04796064d127db59811510fc
            python3 -B /work/scripts/periph/aop/resolve-nextint-kexec.py
            git add drivers/pci/controller/pcie-apple-h9p.c
            GIT_EDITOR=true git cherry-pick --continue
        fi
    fi
done
export AOP_STAGE3_SOURCE="$stage3"
python3 -B - <<"PY"
from pathlib import Path
import os, subprocess
source = os.environ["AOP_STAGE3_SOURCE"]
def original(path):
    return subprocess.check_output(["git", "show", f"{source}:{path}"])
paths = subprocess.check_output([
    "git", "ls-tree", "-r", "--name-only", source, "drivers/soc/apple"
], text=True).splitlines()
paths = [p for p in paths if Path(p).name.startswith(("a9-aop", "apple-a9-aop"))]
paths += ["Documentation/devicetree/bindings/soc/apple/apple,j71t-aop.yaml",
          "Documentation/devicetree/bindings/soc/apple/apple,j71t-aop-observer.yaml",
          "arch/arm64/boot/dts/apple/s8003-j71t-aop.dtsi",
          "arch/arm64/boot/dts/apple/s8003-j71t-aop-observer.dtsi"]
for p in paths:
    Path(p).write_bytes(original(p))
# Copy just the AOP Kconfig blocks, retaining nextint-specific entries.
p = Path("drivers/soc/apple/Kconfig")
text, updated = p.read_text(), original(str(p)).decode()
start, end = "config APPLE_A9_AOP\n", "config APPLE_MAILBOX\n"
text = text[:text.index(start)] + updated[updated.index(start):updated.index(end)] + text[text.index(end):]
p.write_text(text)
# Retain the rest of nextint DT (including USB host and its periph layout).
p = Path("arch/arm64/boot/dts/apple/s8003-j71t.dts")
text = p.read_text()
for reservation in ("/memreserve/ 0x210e00000 0x9b000;", "/memreserve/ 0x858000000 0x400000;"):
    if reservation not in text:
        text = text.replace("/dts-v1/;", "/dts-v1/;\n" + reservation, 1)
p.write_text(text)
PY
git diff --check
if ! git diff --quiet; then
    git add -- drivers/soc/apple/a9-aop* drivers/soc/apple/apple-a9-aop* drivers/soc/apple/Kconfig \
        Documentation/devicetree/bindings/soc/apple/apple,j71t-aop* \
        arch/arm64/boot/dts/apple/s8003-j71t-aop* arch/arm64/boot/dts/apple/s8003-j71t.dts
    git commit -m "soc: apple-a9-aop: carry opt-in stage3 into nextint"
fi
test -z "$(git status --porcelain)"
cp /work/build/kernel-nextint/config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
for key in APPLE_A9_AOP APPLE_A9_AOP_ACTIVE APPLE_A9_AOP_IIO PSTORE PSTORE_RAM PSTORE_CONSOLE KEXEC_FILE DEBUG_INFO_NONE; do
    grep -qx "CONFIG_$key=y" .config
done
make LLVM=1 ARCH=arm64 W=1 -j4 drivers/soc/apple/apple-a9-aop.o \
    drivers/soc/apple/apple-a9-aop-active.o drivers/soc/apple/a9-aop-iio.o \
    drivers/pci/controller/pcie-apple-h9p.o dtbs \
    > /work/build/periph/aop-autorotate/nextint-w1.log 2>&1
if grep -E "warning:|error:" /work/build/periph/aop-autorotate/nextint-w1.log; then exit 1; fi
python3 -B /work/scripts/periph/aop/check-stage23-memory.py --dtb arch/arm64/boot/dts/apple/s8003-j71t.dtb
picked=$(git log --format=%B "$base..HEAD")
grep -Fq a412bc8f04aeb9ac04796064d127db59811510fc <<< "$picked"
grep -Fq 6d449cccb367476701872e683021be769c1f5dca <<< "$picked"
git diff "$base" --binary > /work/build/periph/aop-autorotate/nextint-prep.patch
git log -3 --format="%H %s" > /work/build/periph/aop-autorotate/nextint-COMMITS
cp .config /work/build/periph/aop-autorotate/nextint.config
cp arch/arm64/boot/dts/apple/s8003-j71t.dtb /work/build/periph/aop-autorotate/nextint.dtb
test -z "$(git status --porcelain)"
' > "$ROOT/build/periph/aop-autorotate/nextint-prepare.log" 2>&1
echo 'Prepared a9/aop-nextint-prep; opt-in only. See nextint-prepare.log and aop-autorotate.md.'
