#!/usr/bin/env bash
# Offline only: exact Round 7+8+8b+8c overlay on the same tested integration.
# Only a9/boot may be committed; this integration stays detached and dirty.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round8c"
mkdir -p "$OUT"
"$ROOT/scripts/build-kernel.sh" --run boot '
test "$(git branch --show-current)" = a9/boot
test -z "$(git status --porcelain)"
git merge-base --is-ancestor 24ef187b3091aaeb17455fe2ba3bfcd9f935b603 HEAD
git diff --binary b48c325d1a1dc8c10f821a6b8dff9811cb1f6d40 HEAD > /work/build/boot/round8c/kernel.patch
git rev-parse HEAD > /work/build/boot/round8c/boot-head.txt
base=d5822a7f71cd
if [ ! -d /src/wt/bootr8c-check ]; then
  git worktree add --detach /src/wt/bootr8c-check "$base"
fi
cd /src/wt/bootr8c-check
test "$(git rev-parse HEAD)" = "$(git rev-parse "$base")"
if [ -n "$(git status --porcelain)" ]; then
  git diff --binary > /tmp/round8c-existing.patch
  cmp /tmp/round8c-existing.patch /work/build/boot/round8c/kernel.patch
else
  git apply --check /work/build/boot/round8c/kernel.patch
  git apply /work/build/boot/round8c/kernel.patch
fi
test -z "$(git ls-files --others --exclude-standard)"
cp /work/build/kernel-bootr7-check/config .config
scripts/config -d DEBUG_INFO -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE
make LLVM=1 ARCH=arm64 olddefconfig >/dev/null
git rev-parse HEAD > /work/build/boot/round8c/integration-base.txt
git diff --binary > /work/build/boot/round8c/integration.patch
'
HEAD="$(cat "$OUT/boot-head.txt")"
KERNEL_LOCALVERSION="-a9-boot-r8c-${HEAD:0:10}" \
  "$ROOT/scripts/build-kernel.sh" --tree bootr8c-check
python3 -B -I - "$ROOT" <<'PY'
from pathlib import Path
import gzip
import sys
r = Path(sys.argv[1])
old, new = (r / 'build' / ('kernel-' + n) for n in ('bootr7-check', 'bootr8c-check'))
assert (old / 'config').read_bytes() == (new / 'config').read_bytes(), 'unexpected config delta'
assert (old / 'dtbs/s8003-j71t.dtb').read_bytes() == (new / 'dtbs/s8003-j71t.dtb').read_bytes(), 'unexpected DTB delta'
out = r / 'build/boot/round8c'
assert (out / 'kernel.patch').read_bytes() == (out / 'integration.patch').read_bytes()
assert (out / 'kernel.patch').read_bytes() == (new / 'SOURCE.patch').read_bytes()
head = (out / 'boot-head.txt').read_text().strip()
release = (new / 'RELEASE').read_text().strip()
assert f'-a9-boot-r8c-{head[:10]}' in release
assert b'Linux version ' + release.encode() + b' ' in gzip.decompress((new / 'Image.gz').read_bytes())
print('PASS pinned Round 7+8+8b+8c overlay; unchanged config/DTB; Image embeds', release)
PY
