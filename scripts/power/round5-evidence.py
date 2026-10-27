#!/usr/bin/env python3
"""Summarize supplied round-4 device logs locally; never connects to hardware."""
from collections import Counter
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
logs = ROOT / 'docs/power/logs/2026-10-23-r4-oc'
result = {'source': str(logs.relative_to(ROOT)), 'device_access': 'none', 'trials': {}}
for name in ('boost-r4.jsonl', 'stock-2g-r4.jsonl'):
    path = logs / name
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    samples = [r for r in rows if r.get('event') == 'trial-sample']
    start = next(r for r in rows if r.get('event') == 'trial-start')
    result['trials'][name] = {
        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'workload_command': start['command'], 'samples': len(samples),
        'actual_khz_counts': dict(Counter(r['actual_khz'] for r in samples)),
        'command_counts': dict(Counter(r['guard']['cmd'] for r in samples)),
        'status_counts': dict(Counter(r['guard']['status'] for r in samples)),
        'requested_state_counts': dict(Counter(int(r['guard']['status'], 16) & 15 for r in samples)),
        'actual_state_counts': dict(Counter((int(r['guard']['status'], 16) >> 4) & 15 for r in samples)),
        'failure': next(r['error'] for r in rows if r.get('event') == 'failure'),
    }
status = (logs / 'handoff-status.txt').read_text()
fields = lambda line: dict(item.split('=', 1) for item in line.split() if '=' in item)
states = {int(r['state']): r for r in map(fields, status.splitlines()) if 'state' in r}
result['native_boost'] = {'state': 8, 'psinfo1': states[8]['boot_psinfo1'],
    'psinfo2': states[8]['boot_psinfo2'],
    'max_load': int(states[8]['boot_psinfo2'], 16) >> 40 & 15}
result['stock_all_load'] = {'state': 7, 'max_load': int(states[7]['boot_psinfo2'], 16) >> 40 & 15}
result['slot15_matches_slot2_boot_placeholder'] = all(
    states[15][key] == states[2][key] for key in ('boot_psinfo1', 'boot_psinfo2'))
assert result['slot15_matches_slot2_boot_placeholder']
assert all(t['actual_state_counts'] == {7: t['samples']} and
           t['requested_state_counts'] == {8: t['samples']} for t in result['trials'].values())
result['conclusion'] = ('Software boost reaches the hardware request. Actual state stays at 7. '
    'This is consistent with the native state-8 load=2 restriction; the exact hardware '
    'load/idle gating is not measured. Round 5 uses a separate guarded all-load stock clone.')
output = ROOT / 'docs/power/evidence/overclock-round5.json'
output.write_text(json.dumps(result, indent=2) + '\n')
print(output.relative_to(ROOT))
