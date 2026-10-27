#!/usr/bin/env bash
# Validate the owning-branch build and record portable artifact checksums.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$(cat /work/patches/periph/usb-resume/HEAD)"
python3 -B -I /work/scripts/periph/verify-kernel.py
'
python3 -B -I - "$ROOT" <<'PY'
import gzip
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
out = root / 'build/periph/usb-resume'
kernel = root / 'build/kernel-periph'
head = (root / 'patches/periph/usb-resume/HEAD').read_text().strip()
assert (kernel / 'COMMIT').read_text().split()[0] == head
assert not (kernel / 'DIRTY').read_text().strip()
assert not (kernel / 'SOURCE.patch').read_bytes()
release = (kernel / 'RELEASE').read_text().strip()
assert head[:12] in release and 'dirty' not in release, release
assert b'Linux version ' + release.encode() + b' ' in gzip.decompress((kernel / 'Image.gz').read_bytes())
config = (kernel / 'config').read_text()
for symbol in ('CONFIG_DEBUG_INFO_NONE=y', 'CONFIG_USB_DWC2=y',
               'CONFIG_USB_CONFIGFS_ECM=y', 'CONFIG_APPLE_D2365_CHARGER=y'):
    assert symbol in config.splitlines(), symbol
paths = [kernel / name for name in ('Image.gz', 'dtbs/s8003-j71t.dtb', 'config', 'COMMIT', 'RELEASE')]
paths += sorted((root / 'patches/periph/usb-resume').glob('*.patch'))
paths += [out / 'rootfs-recovery.tar']
(out / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root)}\n' for path in paths))
print('PASS: clean owning-branch Image.gz + J71t DTB, release', release)
PY
