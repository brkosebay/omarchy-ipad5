#!/usr/bin/env bash
# Export only the Round 8d fix on a9/boot; never commit workspace/integration.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/boot/round8d"
"$ROOT/scripts/build-kernel.sh" --run boot '
test "$(git branch --show-current)" = a9/boot
test -z "$(git status --porcelain)"
base=$(git rev-parse bd24e6785e050003481478d798a76695006f7341)
head=$(git rev-parse HEAD)
test "$(git rev-list --count "$base..$head")" = 1
git format-patch --no-signature --zero-commit -o /work/patches/boot/round8d "$base..$head"
printf "%s\n" "$base" > /work/patches/boot/round8d/BASE
printf "%s\n" "$head" > /work/patches/boot/round8d/HEAD
'
python3 -B -I - "$ROOT" <<'PY'
from pathlib import Path
import hashlib
import json
import sys
out = Path(sys.argv[1]) / 'patches/boot/round8d'
patches = sorted(out.glob('*.patch'))
assert len(patches) == 1
(out / 'SERIES').write_text(''.join(p.name + '\n' for p in patches))
(out / 'manifest.json').write_text(json.dumps({
    'base': (out / 'BASE').read_text().strip(),
    'head': (out / 'HEAD').read_text().strip(),
    'branch': 'a9/boot', 'incremental_after': 'patches/boot/round8c',
    'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
    'device_access': False, 'hardware_tested': False,
}, indent=2) + '\n')
PY
