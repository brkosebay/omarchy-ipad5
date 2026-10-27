#!/usr/bin/env bash
# Export only the USB fixes following Round 6, on the owning a9/periph branch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$ROOT/patches/periph/usb-resume"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test "$(git branch --show-current)" = a9/periph
test -z "$(git status --porcelain)"
base=$(git rev-parse 64221b189e)
head=$(git rev-parse HEAD)
test "$(git rev-list --count "$base..$head")" = 2
out=/work/patches/periph/usb-resume
git format-patch --no-signature "$base..$head" -o "$out"
git rev-parse "$base" > "$out/BASE"
git rev-parse "$head" > "$out/HEAD"
git diff --check "$base..$head"
git apply --check --reverse "$out"/*.patch
scripts/checkpatch.pl --strict --no-tree "$out"/*.patch
'
python3 -B -I - "$ROOT" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
import tarfile
root = Path(sys.argv[1])
out = root / 'patches/periph/usb-resume'
patches = sorted(out.glob('*.patch'))
assert len(patches) == 2
(out / 'series').write_text(''.join(p.name + '\n' for p in patches))
(out / 'manifest.json').write_text(json.dumps({
    'branch': 'a9/periph',
    'base': (out / 'BASE').read_text().strip(),
    'head': (out / 'HEAD').read_text().strip(),
    'incremental_after': 'patches/periph/0055-*',
    'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
    'device_access': False,
    'hardware_tested': False,
}, indent=2) + '\n')
# A targeted rootfs deployment, without mounting or editing a live image.
files = root / 'scripts/omarchy/rootfs/files'
archive = root / 'build/periph/usb-resume/rootfs-recovery.tar'
archive.parent.mkdir(parents=True, exist_ok=True)
with tarfile.open(archive, 'w') as tar:
    for name, mode in (('usr/local/sbin/linux-a9-usb-recover', 0o755),
                       ('etc/systemd/system/linux-a9-usb-recover.service', 0o644)):
        path = files / name
        info = tar.gettarinfo(path, arcname=name)
        info.uid = info.gid = 0
        info.uname = info.gname = 'root'
        info.mtime = 0
        info.mode = mode
        with path.open('rb') as stream:
            tar.addfile(info, stream)
print('Exported', archive.relative_to(root))
PY
