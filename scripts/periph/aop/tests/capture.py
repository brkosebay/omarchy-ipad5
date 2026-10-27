#!/usr/bin/env python3
"""Run the real collector against a local fake SSH executable; no network."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix='aop-capture-') as temp:
        root = Path(temp)
        collector = root / 'scripts/periph/aop/capture.sh'
        collector.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'capture.sh', collector)
        ssh = root / 'scripts/omarchy/ssh.sh'
        ssh.parent.mkdir()
        ssh.write_text('''#!/usr/bin/env bash
set -eu
case "$1" in
  *'/proc/device-tree/compatible'*) echo apple,j71t ;;
  'sudo -n test -d '*) exit 0 ;;
  'sudo -n test -r '*) [[ "$AOP_FAKE_CASE" != *missing ]] ;;
  'sudo -n cat /sys/kernel/debug/apple-a9-aop-active/state')
    echo format=1
    [[ "$AOP_FAKE_CASE" == old* ]] || echo 'layout supported=contiguous-v5'
    exit 0 ;;
  'sudo -n cat /sys/kernel/debug/apple-a9-aop-active/fingerprint')
    echo format=1
    [[ "$AOP_FAKE_CASE" != early ]] || exit 0
    [[ "$AOP_FAKE_CASE" != old ]] || exit 0
    [[ "$AOP_FAKE_CASE" == no_snapshot ]] || echo 'layout supported=contiguous-v5'
    [[ "$AOP_FAKE_CASE" != late ]] || exit 0
    echo snapshot_end=1 ;;
  *) exit 0 ;;
esac
''')
        ssh.chmod(0o755)
        for case in ('complete', 'no_snapshot', 'old', 'old_missing',
                     'early', 'late', 'missing'):
            result = subprocess.run(['bash', str(collector), 'active'],
                                    env=dict(os.environ, AOP_CAPTURE_ROUND='aop-r7',
                                             AOP_FAKE_CASE=case), capture_output=True, text=True)
            expected = 2 if case in ('early', 'late', 'missing') else 0
            assert result.returncode == expected, (case, result.returncode, result.stderr)
    print('PASS: collector rejects early/late/missing Round 7 fingerprints; complete/no-snapshot/legacy captures work (SSH locally faked)')


if __name__ == '__main__':
    main()
