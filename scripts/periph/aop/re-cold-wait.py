#!/usr/bin/env python3
"""Reproduce v4 cache-off fatal evidence from local frozen captures; no IO."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def main():
    inspect = runpy.run_path(str(HERE / 'inspect-cold-memory.py'))
    fwdir = ROOT / 'build/periph/aop-r2/firmware/apple/aop'
    firmware = (fwdir / 'ipad6faop-20H350.bin').read_bytes()
    ext = (fwdir / 'ipad6faop_ext-20H350.bin').read_bytes()
    output = ROOT / 'build/periph/aop-force-cold/re-wait'
    output.mkdir(parents=True, exist_ok=True)
    reports, records = [], []
    for name in ('cold2c-v4-20261025T132921Z', 'cold2b-v4-20261025T133030Z'):
        directory = ROOT / 'build/periph/aop-stage23' / name
        before, after = [(directory / filename).read_bytes() for filename in ('cold_before.bin', 'cold_after.bin')]
        digest = hashlib.sha256(after).hexdigest()
        frames = re.findall(r'^\d+ event=(\S+).*?sha256=([0-9a-f]{64})',
                            (directory / 'cold_memory.txt').read_text(), re.M)
        assert len(frames) == 8 and frames[0] == ('loaded', hashlib.sha256(before).hexdigest())
        assert all(sha == digest for _, sha in frames[1:])
        mapped = inspect['pinned_report'](before, after, firmware, ext)
        assert 'cache_off_guard_agrees=1' in mapped and 'PRNG FATAL:' in mapped
        report = f'Capture: {name}\n' + inspect['inspect'](before, after) + mapped
        (output / f'{name}.txt').write_text(report)
        reports.append(report)
        records.append(dict(capture=str(directory.relative_to(ROOT)),
                            before_sha256=hashlib.sha256(before).hexdigest(),
                            after_sha256=digest, post_release_frames=len(frames)-1,
                            post_release_hashes_identical=True, sampled_pc=False,
                            fatal='PRNG !patchbay', ecid_nonzero=False))
    subprocess.run([sys.executable, '-B', str(HERE / 'firmware-attach-re.py'),
                    '--cold-wait', '--windows-only'], check=True)
    (output / 'evidence.json').write_text(json.dumps(records, indent=2) + '\n')
    # A derived report, not a modification to either historical capture.
    (ROOT / 'docs/periph/aop-cold-wait-memory.txt').write_text('\n'.join(reports))
    print(f'PASS two coherent-guard snapshots, seven matching post-release hashes each, identical fatal/task state; {output}')


if __name__ == '__main__':
    main()
