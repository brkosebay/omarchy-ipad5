#!/usr/bin/env python3
"""A/B CPU benchmark at stock vs a guarded tuned clock (orchestrator, root, on device).

Runs the same stress-ng matrixprod benchmark (1 and 2 workers, fixed duration,
--metrics-brief) at stock 1.8 GHz and at TARGET kHz through the a9_tuning
guard, feeding the guard heartbeat during the tuned run and reverting after.
Waits for the hottest sensor to cool below COOL_MILLIC before every run.
Prints one JSON line per run: clock, workers, bogo ops/s (real time), peak temp.

Usage: bench-ab.py [TARGET_KHZ=2160000] [SECONDS=30]
"""
import json, re, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ladder  # noqa: E402  (same directory on the device)

ROOT = Path('/sys')
POLICY = ROOT / 'devices/system/cpu/cpufreq/policy0'
TARGET = int(sys.argv[1]) if len(sys.argv) > 1 else 2160000
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 30
COOL_MILLIC = 60000
WORKERS = (2,)  # owner: benchmark both cores only


def hottest():
    return max(ladder.sensors(ROOT)[0].values())


def cool():
    end = time.monotonic() + 300
    while hottest() >= COOL_MILLIC and time.monotonic() < end:
        time.sleep(2)


def actual_khz():
    return int((POLICY / 'scaling_cur_freq').read_text())


def run(label, workers, tuned):
    cool()
    peak = [hottest()]
    clocks = []
    stop = threading.Event()

    def watch():
        while not stop.is_set():
            if tuned:
                ladder.control(ROOT, 'heartbeat')
            peak[0] = max(peak[0], hottest())
            clocks.append(actual_khz())
            time.sleep(0.5)

    saved = {n: (POLICY / n).read_text().strip() for n in ('scaling_governor', 'scaling_max_freq')}
    try:
        if tuned:
            ladder.ensure_stock(ROOT)
            ladder.control(ROOT, 'arm')
            ladder.control(ROOT, f'configure {TARGET} 0')
            (ROOT / 'devices/system/cpu/cpufreq/boost').write_text('1\n')
            (POLICY / 'scaling_max_freq').write_text(f'{TARGET}\n')
        (POLICY / 'scaling_governor').write_text('performance\n')
        t = threading.Thread(target=watch, daemon=True)
        t.start()
        out = subprocess.run(['stress-ng', '--cpu', str(workers), '--cpu-method', 'matrixprod',
                              '--timeout', f'{SECONDS}s', '--metrics-brief'],
                             capture_output=True, text=True).stdout + ''
        stop.set()
        t.join()
    finally:
        if tuned:
            ladder.control(ROOT, 'revert')
        (POLICY / 'scaling_max_freq').write_text(str(min(int(saved['scaling_max_freq']), 1800000)) + '\n')
        (POLICY / 'scaling_governor').write_text(saved['scaling_governor'] + '\n')
    m = None
    for line in out.splitlines():
        m = re.search(r'metrc:.*\bcpu\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line) or m
    ops = float(m.group(5)) if m else None
    median_clock = sorted(clocks)[len(clocks) // 2] if clocks else None
    print(json.dumps({'run': label, 'workers': workers, 'bogo_ops_per_s': ops,
                      'median_khz': median_clock, 'peak_millic': peak[0]}), flush=True)
    return ops


results = {}
for workers in WORKERS:
    results[('stock', workers)] = run('stock-1800', workers, tuned=False)
    results[('tuned', workers)] = run(f'tuned-{TARGET // 1000}', workers, tuned=True)
for workers in WORKERS:
    s, t = results[('stock', workers)], results[('tuned', workers)]
    if s and t:
        print(json.dumps({'summary_workers': workers, 'stock': s, 'tuned': t,
                          'speedup_pct': round((t / s - 1) * 100, 1)}), flush=True)
