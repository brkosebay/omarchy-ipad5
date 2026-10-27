#!/usr/bin/env bash
# Export the three charging-policy commits after USB resume; no device access.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/periph/charging-policy" "$ROOT/build/periph/charging-policy"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test "$(git branch --show-current)" = a9/periph
test -z "$(git status --porcelain)"
base=$(git rev-parse 46864bf1264874b31f9eff73dda5f1b529ca013a)
head=$(git rev-parse HEAD)
test "$(git rev-list --count "$base..$head")" = 3
out=/work/patches/periph/charging-policy
git format-patch --no-signature "$base..$head" -o "$out"
git rev-parse "$base" > "$out/BASE"
git rev-parse "$head" > "$out/HEAD"
git diff --check "$base..$head"
for patch in "$out"/*.patch; do
 scripts/checkpatch.pl --strict --no-tree "$patch"
done
'
python3 -B -I - "$ROOT" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
out = root / 'patches/periph/charging-policy'
patches = sorted(out.glob('*.patch'))
assert len(patches) == 3
(out / 'series').write_text(''.join(p.name + '\n' for p in patches))
(out / 'manifest.json').write_text(json.dumps({
    'branch': 'a9/periph',
    'base': (out / 'BASE').read_text().strip(),
    'head': (out / 'HEAD').read_text().strip(),
    'incremental_after': 'patches/periph/usb-resume/series',
    'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
    'device_access': False, 'hardware_tested': False, 'workspace_commits': False,
}, indent=2) + '\n')
PY
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
python3 -B -I /work/scripts/periph/check-charging-policy-integration.py
'
