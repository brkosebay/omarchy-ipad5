#!/usr/bin/env python3
"""Record observed offline output and explicitly unmet charge7 requirements."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/charge7'


def main():
    subprocess.run(['python3', '-B', '-I', 'scripts/periph/verify-charge7.py'],
                    cwd=ROOT, check=True, capture_output=True)
    manifest = json.loads((OUT / 'manifest.json').read_text())
    logs = {name: (OUT / name).read_text() for name in
            ('tests.log', 'tristar-tests.log', 'auto-tests.log', 'idbus-tests.log',
             'check.log', 'w1.log', 'checkpatch.log', 'final-build.log', 'verify.log')}
    for name in ('w1.log', 'final-build.log'):
        assert not any('warning:' in line or 'error:' in line for line in logs[name].splitlines())
    assert 'done: 105 dtbs' in logs['final-build.log']
    assert 'PASS four board DTB ceilings' in logs['final-build.log']
    assert '0 errors, 0 warnings, 0 checks' in logs['checkpatch.log']
    sources = sorted((ROOT / 'scripts/periph/kernel').glob('apple*'))
    record = dict(
        recorded_utc=datetime.now(timezone.utc).isoformat(),
        branch='a9/charge', kernel_commit=manifest['kernel_commit'], entry=manifest['entry'],
        payload_sha256=manifest['payload_sha256'], offline='PASS', hardware='NOT RUN',
        w1_new_warnings=0, full_build_warnings=0, dtbs=105,
        unmet=['Verified voltage/collapse-triggered adaptive protection (requirement 4)',
               'Guaranteed physical-detach detection for pulses shorter than all polls (requirement 2)'],
        checkpatch_ignored=['FILE_PATH_CHANGES', 'DT_SPLIT_BINDING_PATCH'],
        sources={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        logs={str((OUT / n).relative_to(ROOT)): hashlib.sha256(t.encode()).hexdigest()
              for n, t in logs.items()})
    (OUT / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    lines = [f"Charge7 results: {record['recorded_utc']}",
             f"Branch: a9/charge {manifest['kernel_commit']}",
             f"Entry: {manifest['entry']}",
             f"Payload SHA256: {manifest['payload_sha256']}", '',
             'PASS: offline implementation/model/compile/package checks below.',
             'NOT RUN: all iPad/device steps.',
             'UNRESOLVED: requirement 4 voltage-triggered protection; sub-poll detach guarantee.',
             'PMU reduction back-off is tested software behavior, not proven droop feedback.', '']
    for name in ('tests.log', 'tristar-tests.log', 'auto-tests.log', 'idbus-tests.log', 'verify.log'):
        lines += [f'[{name}]', logs[name].rstrip(), '']
    lines += ['[W=1]', 'PASS: four freshly rebuilt touched objects, zero warnings/errors.',
              '[checkpatch]', '0 errors, 0 warnings, 0 checks; aggregate-only ignores: FILE_PATH_CHANGES, DT_SPLIT_BINDING_PATCH.',
              '[build/package]', 'PASS: clean Image.gz, 105 DTBs, debug info off, four DT ceilings, manifest hashes.',
              'PASS: command line unchanged except entry name; exactly one ANS keep-power; no debug shell.',
              '[watcher fixture]', 'PASS: fake sysfs in Docker, two-second bound; no device access.',
              '[Ghidra persistence]', 'Post-rename save failed; renames were not confirmed saved. Shared server left running.', '']
    (ROOT / 'docs/periph/charge7-results.txt').write_text('\n'.join(lines))
    print('PASS charge7 report written; unmet hardware requirements preserved explicitly')


if __name__ == '__main__':
    main()
