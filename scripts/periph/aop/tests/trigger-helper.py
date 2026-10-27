#!/usr/bin/env python3
"""Run the real orchestrator helper with local fake SSH; never contact a device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    fixture = ROOT / 'build/periph/aop-stage23/cold2c-v4-20261025T132921Z'
    assert (fixture / 'running.dtb').exists() and (fixture / 'iomem.txt').exists()
    with tempfile.TemporaryDirectory(prefix='aop-trigger-helper-') as temp:
        work = Path(temp)
        ssh = work / 'ssh'
        ssh.write_text('''#!/usr/bin/env python3
import os,sys,time
from pathlib import Path
cmd=sys.argv[1]; case=os.environ['AOP_FAKE_CASE']; out=Path(os.environ['AOP_CAPTURE_DIR'])
with (out/'calls.txt').open('a') as f: f.write(repr(cmd)+'\\n')
fixture=Path(os.environ['AOP_FIXTURE'])
if cmd.startswith('set -eu'):
    if case=='preflight-fail': sys.exit(2)
    print('defer=2 triggered=0')
elif 'dmesg --follow --raw' in cmd:
    if case=='witness-dead': sys.exit(1)
    if case!='no-marker': print('AOP live-console-ready',flush=True)
    time.sleep(30)
elif 'tee /sys/bus/platform/devices/210800000.aop/cold_release' in cmd:
    (out/'triggered').touch(); print('1')
elif '/sys/firmware/fdt' in cmd:
    sys.stdout.buffer.write((fixture/'running.dtb').read_bytes())
elif '/proc/iomem' in cmd:
    print((fixture/'iomem.txt').read_text())
elif cmd.startswith('for i in') or cmd.endswith('/state'):
    print('format=1 phase=failed result=failed-no-hello')
''')
        ssh.chmod(0o755)
        for case in ('preflight-fail', 'witness-dead', 'no-marker', 'complete'):
            out = work / case
            out.mkdir()
            result = subprocess.run(['bash', str(ROOT / 'scripts/periph/aop/trigger-cold.sh'), case],
                env=dict(os.environ, OMARCHY_SSH=str(ssh), AOP_CAPTURE_DIR=str(out),
                         AOP_FAKE_CASE=case, AOP_FIXTURE=str(fixture)),
                capture_output=True, text=True, timeout=20)
            assert (result.returncode == 0) == (case == 'complete'), (case, result.stderr)
            assert (out / 'triggered').exists() == (case == 'complete')
            calls = (out / 'calls.txt').read_text()
            assert calls.count('tee /sys/bus/platform/devices/210800000.aop/cold_release') == (case == 'complete')
            if case == 'complete':
                assert (out / 'pre-trigger-memory-check.txt').read_text().count('PASS') == 1
                assert 'AOP live-console-ready' in (out / 'console-live.txt').read_text()
    print('PASS real trigger helper: preflight, live host witness and memory check precede the only trigger; missing/dead witness refuses release; post-trigger collection retained (SSH locally faked)')


if __name__ == '__main__':
    main()
