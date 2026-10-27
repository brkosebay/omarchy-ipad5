#!/usr/bin/env python3
"""Commit only the reviewed Round 5 files on a9/periph, never the host repo."""
from pathlib import Path
import subprocess
import textwrap

if Path.cwd() != Path('/src/wt/periph'):
    raise SystemExit('wrong kernel tree')
if subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() != 'a9/periph':
    raise SystemExit('wrong branch')
groups = [
    ('power: supply: apple: allow fallback charging and owner input overrides',
     'Round 4 selected zero battery current at an ordinary USB budget and rejected the previously working owner request above the detected allowance. Permit a conservative 400 mA battery ceiling at nominal 500 mA input, still bounded by calibrated input regulation and the hardware cap. Restore positive input_current_limit writes as explicit owner authorization up to 2400 mA. Keep detected USB current truthful, require live VBUS and all battery/status guards, and revoke the override on guard failure or PM quiesce. Retain checked transactions, rollback and fault latches.',
     ['drivers/power/supply/apple_j71t_charger.c']),
    ('power: supply: bq27xxx: omit unavailable cached charge samples',
     'An optional cached charge-now read error must not abort the complete battery uevent. Return ENODATA for an unavailable cached sample while retaining the raw cache error for diagnostics. Preserve valid charge values and recovery, as well as the error behavior of live-read profiles.',
     ['drivers/power/supply/bq27xxx_battery.c']),
]
expected = {p for _, _, paths in groups for p in paths}
changed = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], text=True).splitlines())
if changed != expected:
    raise SystemExit(f'unexpected changed paths: {changed ^ expected}')
subprocess.run(['git', 'diff', '--check'], check=True)
for subject, body, paths in groups:
    subprocess.run(['git', 'add', '--', *paths], check=True)
    subprocess.run(['git', 'commit', '-m', subject, '-m', textwrap.fill(body, 72), '--', *paths], check=True)
assert not subprocess.check_output(['git', 'status', '--porcelain'], text=True).strip()
