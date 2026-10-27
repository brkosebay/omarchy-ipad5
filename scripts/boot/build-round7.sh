#!/usr/bin/env bash
# Offline only. Pin the tested Round 6 integration and overlay only Round 7.
# No integration branch/commit, device access, or moving workstream heads.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round7"
mkdir -p "$OUT"
"$ROOT/scripts/build-kernel.sh" --run boot '
test "$(git branch --show-current)" = a9/boot
test -z "$(git status --porcelain)"
git merge-base --is-ancestor b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40 HEAD
git diff --binary b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40 HEAD > /work/build/boot/round7/kernel.patch
git rev-parse HEAD > /work/build/boot/round7/boot-head.txt
base=d5822a7f71
if [ ! -d /src/wt/bootr7-check ]; then
  git worktree add --detach /src/wt/bootr7-check "$base"
fi
cd /src/wt/bootr7-check
test "$(git rev-parse HEAD)" = "$(git rev-parse "$base")"
if [ -n "$(git status --porcelain)" ]; then
  git diff --binary > /tmp/round7-existing.patch
  cmp /tmp/round7-existing.patch /work/build/boot/round7/kernel.patch
else
  git apply --check /work/build/boot/round7/kernel.patch
  git apply /work/build/boot/round7/kernel.patch
fi
# The overlay is the entire source delta; reject untracked surprises too.
test -z "$(git ls-files --others --exclude-standard)"
cp /work/build/kernel-bootr6/config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
git rev-parse HEAD > /work/build/boot/round7/integration-base.txt
git diff --binary > /work/build/boot/round7/integration.patch
'
"$ROOT/scripts/build-kernel.sh" --tree bootr7-check
python3 -B -I - "$ROOT" <<'PY'
from pathlib import Path
import hashlib
import sys
root = Path(sys.argv[1])
old, new = (root / 'build' / ('kernel-' + x) for x in ('bootr6', 'bootr7-check'))
assert (old / 'config').read_bytes() == (new / 'config').read_bytes(), 'unexpected config delta'
assert (old / 'dtbs/s8003-j71t.dtb').read_bytes() == (new / 'dtbs/s8003-j71t.dtb').read_bytes(), 'unexpected DTB delta'
out = root / 'build/boot/round7'
assert (out / 'kernel.patch').read_bytes() == (out / 'integration.patch').read_bytes(), 'unexpected source delta'
assert (new / 'Image.gz').stat().st_size > 0
print('PASS pinned integration: exact Round 7 overlay, unchanged Round 6 config/DTB')
for p in (new / 'Image.gz', new / 'dtbs/s8003-j71t.dtb', out / 'integration.patch'):
    print(hashlib.sha256(p.read_bytes()).hexdigest(), p.relative_to(root))
PY
