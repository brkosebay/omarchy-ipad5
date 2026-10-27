#!/usr/bin/env python3
"""Commit the three reviewed Round 6 changes on the isolated kernel branch."""
from pathlib import Path
import subprocess
import textwrap

if Path.cwd() != Path('/src/wt/periph'):
    raise SystemExit('wrong kernel tree')
if subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() != 'a9/periph':
    raise SystemExit('wrong branch')
groups = [
    ('power: supply: bq27xxx: distinguish cache warmup from runtime faults',
     'Track the first complete cache-only safety sample. Until flags, capacity, current, voltage and temperature succeed, expose ENODATA including unknown presence, keep raw errors for debug and retry reads after one second. Optional sample failures do not block readiness. After readiness retain runtime error reporting and the regular refresh interval. This avoids treating an initial HDQ failure as a permanently missing battery without inventing measurements.',
     ['drivers/power/supply/bq27xxx_battery.c', 'include/linux/power/bq27xxx_battery.h']),
    ('power: supply: retry unavailable thermal samples quietly',
     'Translate a missing power-supply temperature sample from ENODATA to the thermal core EAGAIN convention. The core will poll again without a startup error message or broken-zone backoff. Preserve real transport errors and successful temperature conversion.',
     ['drivers/power/supply/power_supply_core.c']),
    ('power: supply: apple: retry J71t policy while the gauge warms up',
     'A first-pass absent provider or unavailable cached battery sample must defer the guarded policy instead of setting its permanent fault latch. Keep one-second reevaluation so the ordinary 500 mA policy applies automatically once the gauge supplies valid safety data. Log deferred decisions on transition and every thirty seconds; keep successful budget logs, owner authorization, safety bounds, checked transactions, rollback and genuine fault latches.',
     ['drivers/power/supply/apple_j71t_charger.c']),
]
expected = {p for _, _, paths in groups for p in paths}
changed = set(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], text=True).splitlines())
if changed != expected:
    raise SystemExit(f'unexpected paths: {changed ^ expected}')
subprocess.run(['git', 'diff', '--check'], check=True)
for subject, body, paths in groups:
    subprocess.run(['git', 'add', '--', *paths], check=True)
    subprocess.run(['git', 'commit', '-m', subject, '-m', textwrap.fill(body, 72), '--', *paths], check=True)
assert not subprocess.check_output(['git', 'status', '--porcelain'], text=True).strip()
