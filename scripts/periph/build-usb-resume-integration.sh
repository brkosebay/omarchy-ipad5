#!/usr/bin/env bash
# Exact round8e overlay + USB fix in a new detached tree; never alter integ/boot.
# --check-only replays the patches in a temporary index without another checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK_ONLY=0
case "${1:-}" in
  --check-only) CHECK_ONLY=1 ;;
  '') ;;
  *) echo "usage: $0 [--check-only]" >&2; exit 2 ;;
esac
OUT="$ROOT/build/periph/usb-resume"
mkdir -p "$OUT"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test "$(git rev-parse HEAD)" = "$(cat /work/patches/periph/usb-resume/HEAD)"
test -z "$(git status --porcelain)"
test "$(cat /work/build/boot/round8e/boot-head.txt)" = "$(git rev-parse bfe22c60a0)"
python3 -B -I - <<'"'"'PY'"'"'
import os
from pathlib import Path
import subprocess
import tempfile
out = Path("/work/build/periph/usb-resume")
patches = sorted(Path("/work/patches/periph/usb-resume").glob("*.patch"))
assert len(patches) == 2
with tempfile.TemporaryDirectory() as temp:
    env = dict(os.environ, GIT_INDEX_FILE=temp + "/index")
    def git(*args):
        return subprocess.check_output(["git", *args], env=env)
    # Check the current integration too, without changing its worktree/index.
    for base, boot in (("a9/integ", False), ("d5822a7f71cd", True)):
        git("read-tree", base)
        if boot:
            git("apply", "--cached", "--whitespace=error", "/work/build/boot/round8e/kernel.patch")
        for patch in patches:
            git("apply", "--cached", "--whitespace=error", str(patch))
        for name in ("drivers/usb/dwc2/gadget.c", "drivers/usb/gadget/function/f_ecm.c"):
            assert git("show", ":" + name) == Path(name).read_bytes(), name
        print("PASS: exact USB patch replay on", base, "+ round8e" if boot else "")
        if boot:
            (out / "integration.patch").write_bytes(git("diff", "--cached", "--binary", base))
PY
'
[ "$CHECK_ONLY" = 0 ] || exit 0

"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
base=$(git rev-parse d5822a7f71cd)
if [ ! -d /src/wt/usbresume-check ]; then
  git worktree add --detach /src/wt/usbresume-check "$base"
fi
cd /src/wt/usbresume-check
test "$(git rev-parse HEAD)" = "$base"
test -z "$(git branch --show-current)"
test -z "$(git ls-files --others --exclude-standard)"
if [ -n "$(git status --porcelain)" ]; then
  git diff --binary > /tmp/usb-resume-existing.patch
  cmp /tmp/usb-resume-existing.patch /work/build/periph/usb-resume/integration.patch
else
  git apply --check /work/build/periph/usb-resume/integration.patch
  git apply /work/build/periph/usb-resume/integration.patch
fi
cp /work/build/kernel-bootr8e-check/config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
cmp .config /work/build/kernel-bootr8e-check/config
'
HEAD=$(cat "$ROOT/patches/periph/usb-resume/HEAD")
KERNEL_LOCALVERSION="-a9-usb-resume-${HEAD:0:10}" \
  "$ROOT/scripts/build-kernel.sh" --tree usbresume-check
cmp "$OUT/integration.patch" "$ROOT/build/kernel-usbresume-check/SOURCE.patch"
cmp "$ROOT/build/kernel-bootr8e-check/dtbs/s8003-j71t.dtb" \
  "$ROOT/build/kernel-usbresume-check/dtbs/s8003-j71t.dtb"
