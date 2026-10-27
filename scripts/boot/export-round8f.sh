#!/usr/bin/env bash
# Export only the Round 8f fix on a9/boot; never commit workspace/integration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/boot/round8f" "$ROOT/build/boot/round8f"
"$ROOT/scripts/build-kernel.sh" --run boot '
test "$(git branch --show-current)" = a9/boot
test -z "$(git status --porcelain)"
base=$(git rev-parse bfe22c60a0035c23d3ff711fa82e69b8e0b8db92)
head=$(git rev-parse HEAD)
test "$(git rev-list --count "$base..$head")" = 1
git format-patch --no-signature --zero-commit -o /work/patches/boot/round8f "$base..$head" > /work/build/boot/round8f/exported-patch-name.txt
printf "%s\n" "$base" > /work/patches/boot/round8f/BASE
printf "%s\n" "$head" > /work/patches/boot/round8f/HEAD
'
python3 -B -I - "$ROOT" <<'PY'
from pathlib import Path
import hashlib
import json
import sys
out = Path(sys.argv[1]) / 'patches/boot/round8f'
selected = Path((Path(sys.argv[1]) / 'build/boot/round8f/exported-patch-name.txt').read_text().strip()).name
old = json.loads((out / 'manifest.json').read_text()) if (out / 'manifest.json').exists() else {'patches': {}}
for p in out.glob('*.patch'):
    if p.name != selected:
        assert old['patches'].get(p.name) == hashlib.sha256(p.read_bytes()).hexdigest(), p
        p.unlink()  # only this task's previously manifested export
patches = sorted(out.glob('*.patch'))
assert len(patches) == 1
(out / 'SERIES').write_text(''.join(p.name + '\n' for p in patches))
(out / 'manifest.json').write_text(json.dumps({
    'base': (out / 'BASE').read_text().strip(),
    'head': (out / 'HEAD').read_text().strip(),
    'branch': 'a9/boot', 'incremental_after': 'patches/boot/round8e',
    'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
    'device_access': False, 'hardware_tested': False,
}, indent=2) + '\n')
PY
