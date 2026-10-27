#!/usr/bin/env python3
"""Extract the first complete incoming replay, excluding recursive pstore logs."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/boot/round8e/analysis')
    args = parser.parse_args()
    source = ROOT / 'docs/boot/round8d-chain-boot8-fail/netconsole.txt'
    replay = False
    rows = []
    for lineno, line in enumerate(source.read_text().splitlines(), 1):
        # Exactly one logger wrapper: deeper wrappers belong to retained boots.
        if line.count('[omarchy-rd]:') != 1:
            continue
        outer = re.match(r'^(\d\d:\d\d:\d\d) \[\s*([\d.]+)\] \[omarchy-rd\]: (.*)', line)
        if not outer:
            continue
        wall, elapsed, payload = outer.groups()
        if payload == 'linux-a9: incoming NVMe log BEGIN':
            replay = True
            continue
        if replay and payload == 'linux-a9: incoming NVMe log END':
            break
        if not replay:
            continue
        inner = re.match(r'^\[\s*([\d.]+)\] (.*)', payload)
        if inner:
            rows.append({'line': lineno, 'wall': wall, 'replay_time': elapsed,
                         'boot_time': inner[1], 'message': inner[2]})
    assert rows and rows[-1]['line'] < 4298
    port = [r for r in rows if 'Apple ANS: STEP port 0 ' in r['message']]
    failures = [r for r in port if ' FAIL ' in r['message']]
    assert len(failures) == 2 and all('link-up FAIL -110 last=0x4' in r['message'] for r in failures)
    assert not any('full-bringup-done' in r['message'] for r in port)
    assert not any(re.search(r'^nvme (?:nvme\d|0000:01:00.0)', r['message']) for r in rows)
    passes = {}
    for path in sorted((ROOT / 'build/boot/round8d/device/handoff-chain').glob('boot*/dmesg.txt')):
        text = path.read_text()
        chosen = [line for line in text.splitlines() if re.search(
            r'STEP port 0 (link-up (wait|done|FAIL)|full-bringup-done)|health gate partition/data', line)]
        assert len(chosen) == 4 and any('result 0 (passed)' in line for line in chosen)
        times = [float(re.match(r'^\[\s*([\d.]+)\]', line)[1]) for line in chosen[:2]]
        passes[path.parent.name] = {'link_poll_seconds': round(times[1] - times[0], 6), 'lines': chosen}
    assert len(passes) == 7
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'incoming-boot8.txt').write_text(''.join(
        f"raw:{r['line']} {r['wall']} replay={r['replay_time']} boot={r['boot_time']} {r['message']}\n"
        for r in rows))
    (args.out / 'incoming-boot8-steps.txt').write_text(''.join(
        f"raw:{r['line']} [{float(r['boot_time']):12.6f}] {r['message']}\n" for r in port))
    (args.out / 'comparison.json').write_text(json.dumps({
        'source': str(source.relative_to(ROOT)), 'failed_boot': 8,
        'extraction': 'first complete top-level incoming replay, exactly one logger wrapper',
        'failures': failures, 'passing_boots': passes,
        'conclusion': 'two link training timeouts before enumeration; no controller/RDY/admin failure observed',
        'intermittency': 'cause unproven; retained port state/power sequencing is a hypothesis, not an ANS firmware finding',
    }, indent=2) + '\n')
    for r in failures:
        print(f"raw:{r['line']} boot={r['boot_time']} {r['message']}")
    print('PASS: seven first-attempt link/health successes; no incoming boot-8 controller enumeration')


if __name__ == '__main__':
    main()
