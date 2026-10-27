#!/usr/bin/env python3
"""Check the complete final package manifest and isolated variant selectors."""
import hashlib
import json
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / 'build/periph/aop-stage2d'


def main():
    m = json.loads((OUT / 'manifest.json').read_text())
    assert m['series'] == 's2d' and m['branch'] == 'a9/tri49a'
    assert not m['uncommitted'] and not m['hardware_execution_verified']
    expected = {(2, 'd'): 1, (3, 'd'): 1, (2, 'e'): 2,
                (2, 'f'): 3, (2, 'g'): 4, (2, 'h'): 5}
    assert {(e['stage'], e['variant']): e['wake'] for e in m['entries']} == expected
    for relative, info in m['files'].items():
        path = (ROOT / relative).resolve()
        path.relative_to(ROOT / 'build')
        data = path.read_bytes()
        assert len(data) == info['bytes'] and hashlib.sha256(data).hexdigest() == info['sha256'], relative
    for e in m['entries']:
        entry = ROOT / e['entry']
        cmdline = (entry / 'cmdline').read_text().strip()
        tokens = shlex.split(cmdline)
        assert len(cmdline.encode()) == e['cmdline_bytes'] < 1000
        assert tokens.count(f'apple_a9_aop.attach_stage={e["stage"]}') == 1
        assert tokens.count(f'apple_a9_aop.wake={e["wake"]}') == 1
        assert not any(t.startswith('apple_a9_aop.cycle=') for t in tokens)
        assert not (entry / 'DIRTY').read_text().strip()
        assert (entry / 'COMMIT').read_text().split()[0] == m['kernel_commit']
        for line in (entry / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ', 1)
            assert hashlib.sha256((entry / name).read_bytes()).hexdigest() == digest
    print(f'PASS final package: six isolated entries, complete file hashes, clean {m["kernel_commit"]}, all cmdlines <1000 bytes')


if __name__ == '__main__':
    main()
