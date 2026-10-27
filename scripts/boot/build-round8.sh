#!/usr/bin/env bash
# Offline only: overlay Round 7+8 on the exact tested Round 6 integration.
# No integration branch/commit and no device access. Keep the cold gate intact.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round8"
mkdir -p "$OUT"
"$ROOT/scripts/build-kernel.sh" --run boot '
test "$(git branch --show-current)" = a9/boot
test -z "$(git status --porcelain)"
git merge-base --is-ancestor 4e9a87e9ef HEAD
git diff --binary b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40 HEAD > /work/build/boot/round8/kernel.patch
git rev-parse HEAD > /work/build/boot/round8/boot-head.txt
base=d5822a7f71cd
if [ ! -d /src/wt/bootr8-check ]; then
  git worktree add --detach /src/wt/bootr8-check "$base"
fi
cd /src/wt/bootr8-check
test "$(git rev-parse HEAD)" = "$(git rev-parse "$base")"
if [ -n "$(git status --porcelain)" ]; then
  git diff --binary > /tmp/round8-existing.patch
  cmp /tmp/round8-existing.patch /work/build/boot/round8/kernel.patch
else
  git apply --check /work/build/boot/round8/kernel.patch
  git apply /work/build/boot/round8/kernel.patch
fi
test -z "$(git ls-files --others --exclude-standard)"
cp /work/build/kernel-bootr7-check/config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
git rev-parse HEAD > /work/build/boot/round8/integration-base.txt
git diff --binary > /work/build/boot/round8/integration.patch
'
"$ROOT/scripts/build-kernel.sh" --tree bootr8-check
python3 -B -I - "$ROOT" <<'PY'
from pathlib import Path
import sys
r = Path(sys.argv[1])
old, new = (r / 'build' / ('kernel-' + n) for n in ('bootr7-check', 'bootr8-check'))
assert (old / 'config').read_bytes() == (new / 'config').read_bytes(), 'unexpected config delta'
assert (old / 'dtbs/s8003-j71t.dtb').read_bytes() == (new / 'dtbs/s8003-j71t.dtb').read_bytes(), 'unexpected DTB delta'
out = r / 'build/boot/round8'
assert (out / 'kernel.patch').read_bytes() == (out / 'integration.patch').read_bytes()
print('PASS pinned integration: exact Round 7+8 overlay; unchanged tested config, write guard and j71t DTB')
PY
