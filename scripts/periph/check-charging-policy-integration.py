#!/usr/bin/env python3
"""Replay USB + charging patches in private indexes; never change other trees."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path('/work')
OUT = ROOT / 'build/periph/charging-policy'


def main():
    patches = list(sorted((ROOT / 'patches/periph/usb-resume').glob('*.patch')))
    charging = ROOT / 'patches/periph/charging-policy'
    patches += [charging / name for name in (charging / 'series').read_text().splitlines()]
    assert len(patches) == 5
    targets = [('a9/integ', None)]
    for round_name in ('round8e', 'round8f'):
        overlay = ROOT / f'build/boot/{round_name}/integration.patch'
        if overlay.exists():
            targets.append(('d5822a7f71cd', overlay))
    OUT.mkdir(parents=True, exist_ok=True)
    records = []
    with tempfile.TemporaryDirectory() as temp:
        env = dict(os.environ, GIT_INDEX_FILE=temp + '/index')

        def git(*args):
            return subprocess.check_output(['git', *args], env=env)

        for base, overlay in targets:
            resolved = git('rev-parse', base).decode().strip()
            git('read-tree', resolved)
            if overlay:
                git('apply', '--cached', '--whitespace=error', str(overlay))
            for patch in patches:
                # Idempotent when the orchestrator has already integrated a fix.
                check = subprocess.run(['git', 'apply', '--cached', '--reverse', '--check',
                                        str(patch)], env=env, capture_output=True)
                if check.returncode:
                    git('apply', '--cached', '--whitespace=error', str(patch))
            for name in ('apple_j71t_charger.c', 'apple_tristar_power.c'):
                assert git('show', ':drivers/power/supply/' + name) == Path(
                    'drivers/power/supply', name).read_bytes(), name
            for prop in (b'apple,usb-input-limit-max-microamp = <2400000>',
                         b'apple,owner-input-limit-microamp = <2400000>'):
                assert prop in git('show', ':arch/arm64/boot/dts/apple/s8003-j71t-periph.dtsi')
            label = overlay.parent.name if overlay else 'integ'
            artifact = OUT / f'integration-{label}.patch'
            artifact.write_bytes(git('diff', '--cached', '--binary', resolved))
            records.append({'base': resolved,
                            'overlay': str(overlay) if overlay else None,
                            'result': artifact.name,
                            'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()})
            print('PASS: private-index replay:', base, label)
    (OUT / 'integration.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
