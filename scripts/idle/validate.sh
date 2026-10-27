#!/usr/bin/env bash
# Offline checks only; requires the completed standalone idle build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
python3 scripts/idle/test_measure.py
bash -n scripts/idle/build.sh scripts/idle/validate.sh scripts/integrate.sh
python3 - <<'PY'
import hashlib
from pathlib import Path

out = Path('build/kernel-idle')
print('Built commit:', (out / 'COMMIT').read_text().strip())
assert not (out / 'DIRTY').read_text().strip(), 'kernel tree dirty at build'
config = set((out / 'config').read_text().splitlines())
for line in Path('patches/idle/idle.config').read_text().splitlines():
    if line.startswith('CONFIG_'):
        assert line in config, f'missing requested option: {line}'
assert 'CONFIG_DEBUG_INFO_NONE=y' in config
dtbs = list((out / 'dtbs').glob('*.dtb'))
assert len(dtbs) >= 4, 'missing Apple DTBs'
print('DTBs:', len(dtbs))
for path in (out / 'Image.gz', out / 'dtbs/s8003-j71t.dtb'):
    print(hashlib.sha256(path.read_bytes()).hexdigest(), path)
PY
"$ROOT/scripts/build-kernel.sh" --run idle '
set -e
test "$(git branch --show-current)" = a9/idle
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$(cut -d " " -f1 /work/build/kernel-idle/COMMIT)"
python3 /work/scripts/idle/test-entry-guards.py --source drivers/cpuidle/cpuidle-apple-a9-idle.c
git diff --check hoolock
git log --format="%H %s" hoolock..HEAD
idle_index=$(mktemp /tmp/a9-idle-index.XXXXXX)
rm "$idle_index"
export GIT_INDEX_FILE="$idle_index"
trap '\''rm -f "$GIT_INDEX_FILE"'\'' EXIT
git read-tree hoolock
while IFS= read -r patch; do
    git apply --cached "/work/patches/idle/$patch"
done < /work/patches/idle/series
expected=$(git rev-parse HEAD^{tree})
actual=$(git write-tree)
test "$actual" = "$expected"
echo "Exported series reproduces final tree: $actual"
if git rev-parse --verify a9/power >/dev/null 2>&1; then
    echo "Reverse-order merge with a9/power: $(git rev-parse a9/power)"
    git merge-tree --write-tree a9/idle a9/power
fi
'
echo 'Offline checks passed. Physical wake, runtime PM and energy gains remain untested.'
