#!/usr/bin/env bash
# Validate the exported clean Image/DTB and record hashes for this handoff.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/build-kernel.sh" --run periph '
set -euo pipefail
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$(cat /work/patches/periph/charging-policy/HEAD)"
python3 -B -I /work/scripts/periph/verify-kernel.py
pmic=/soc/i2c@20a110000/pmic@3c/charger@180
dtb=/work/build/kernel-periph/dtbs/s8003-j71t.dtb
test "$(fdtget -t u "$dtb" "$pmic" apple,usb-input-limit-max-microamp)" = 2400000
test "$(fdtget -t u "$dtb" "$pmic" apple,owner-input-limit-microamp)" = 2400000
'
python3 -B -I - "$ROOT" <<'PY'
import gzip
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
kernel = root / 'build/kernel-periph'
patches = root / 'patches/periph/charging-policy'
out = root / 'build/periph/charging-policy'
head = (patches / 'HEAD').read_text().strip()
assert (kernel / 'COMMIT').read_text().split()[0] == head
assert not (kernel / 'DIRTY').read_text().strip()
assert not (kernel / 'SOURCE.patch').read_bytes()
release = (kernel / 'RELEASE').read_text().strip()
assert head[:12] in release and 'dirty' not in release
assert b'Linux version ' + release.encode() + b' ' in gzip.decompress((kernel / 'Image.gz').read_bytes())
config = (kernel / 'config').read_text().splitlines()
for option in ('CONFIG_DEBUG_INFO_NONE=y', 'CONFIG_DYNAMIC_DEBUG=y',
               'CONFIG_USB_DWC2=y', 'CONFIG_USB_CONFIGFS_ECM=y',
               'CONFIG_APPLE_D2365_CHARGER=y'):
    assert option in config, option
paths = [kernel / name for name in ('Image.gz', 'dtbs/s8003-j71t.dtb',
                                   'config', 'COMMIT', 'RELEASE', 'DIRTY', 'SOURCE.patch')]
paths += [p for p in sorted(patches.iterdir()) if p.is_file()]
paths += [root / 'scripts/periph' / name for name in (
    'charging-cause-effect.py', 'charging-policy-edit.py',
    'build-charging-policy.sh', 'build-charging-policy-integration.sh',
    'check-charging-policy-build.sh',
    'export-charging-policy.sh', 'check-charging-policy-integration.py',
    'kernel/apple_j71t_charger.c', 'kernel/apple_tristar_power.c',
    'test-charging.py', 'test-round4-source.py')]
paths += [out / name for name in ('build.log', 'export.log', 'regression.log',
                                   'binding-doc.log', 'integration-check.log',
                                   'integration.json', 'commits.txt', 'validation.json')]
paths += sorted(out.glob('integration-*.patch'))
(out / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(root)}\n' for p in paths))
print('PASS: clean W=1 arm64 LLVM Image, linked drivers, ceiling DTB and embedded release', release)
PY
