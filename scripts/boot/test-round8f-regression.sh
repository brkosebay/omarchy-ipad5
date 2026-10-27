#!/usr/bin/env bash
# Mutate only a temporary source fixture; neither the kernel nor device changes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/boot/round8f"
mkdir -p "$OUT"
docker run --rm -i --platform linux/arm64 -v linux-a9-src:/src:ro -v "$ROOT:/work:ro" \
 linux-a9-kbuild python3 -B -I - >"$OUT/deep-regression.log" 2>&1 <<'PY'
from pathlib import Path
import subprocess
import tempfile
source=Path('/src/wt/boot')
file=Path('drivers/pci/controller/pcie-apple-h9p.c')
cases=[('short-off','msleep(1000);','msleep(100);','test_deep_gen1_long_hold_stability_loss_and_isolation'),
       ('no-stability','sample < 100','sample < 0','test_deep_gen1_long_hold_stability_loss_and_isolation'),
       ('lift-ceiling','pcie && pcie->ans_gen1','pcie && false','test_gen1_ceiling_survives_generic_failed_link_quirk')]
for label,old,new,test in cases:
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp)
        (root/file).parent.mkdir(parents=True)
        s=(source/file).read_text()
        assert s.count(old)==1
        (root/file).write_text(s.replace(old,new))
        (root/'include').symlink_to(source/'include',target_is_directory=True)
        run=subprocess.run(['python3','-B','-I','/work/scripts/boot/test-ans-handoff-offline.py',
                            '--source',str(root),'HandoffTests.'+test],capture_output=True,text=True)
        assert run.returncode and 'Assertion' in run.stderr and 'FAILED (errors=1)' in run.stderr,run.stdout+run.stderr
        print('PASS negative control:',label,'was rejected by production-C assertion')
PY
cat "$OUT/deep-regression.log"
