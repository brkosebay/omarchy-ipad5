#!/usr/bin/env python3
"""On-device current/residency recorder. No policy writes unless --backlight-off."""
import argparse
from contextlib import contextmanager
import json
import math
from pathlib import Path
import signal
import statistics
import time

SYS = Path('/sys')
PROC = Path('/proc')


def read(path):
    try:
        value = path.read_text().strip()
        try:
            return int(value)
        except ValueError:
            return value
    except OSError:
        return None


def files(pattern):
    return {str(p): read(p) for p in sorted(SYS.glob(pattern))}


def snapshot():
    paths = [
        'class/power_supply/*/type', 'class/power_supply/*/status',
        'class/power_supply/*/current_now', 'class/power_supply/*/voltage_now',
        'class/power_supply/*/charge_now', 'class/power_supply/*/capacity',
        'class/power_supply/*/temp', 'class/power_supply/*/online',
        'class/thermal/thermal_zone*/temp',
        'class/backlight/*/brightness', 'class/backlight/*/actual_brightness',
        'class/backlight/*/bl_power',
        'devices/system/cpu/cpu*/cpuidle/state*/*',
        'devices/system/cpu/cpuidle/current_driver',
        'devices/system/cpu/cpu*/online',
        'devices/system/cpu/cpufreq/policy*/scaling_cur_freq',
        'devices/system/cpu/cpufreq/policy*/scaling_governor',
    ]
    values = {}
    for pattern in paths:
        values.update(files(pattern))
    stat = read(PROC / 'stat')
    cpu_ticks = {}
    if isinstance(stat, str):
        for line in stat.splitlines():
            words = line.split()
            if words and words[0].startswith('cpu'):
                cpu_ticks[words[0]] = list(map(int, words[1:]))
    return {'monotonic_s': time.monotonic(), 'epoch_s': time.time(),
            'values': values, 'cpu_ticks': cpu_ticks}


def endpoints(out, name):
    data = {'cmdline': read(PROC / 'cmdline'),
            'interrupts': read(PROC / 'interrupts'),
            'genpd': read(SYS / 'kernel/debug/pm_genpd/pm_genpd_summary'),
            'time_in_state': files('devices/system/cpu/cpufreq/policy*/stats/time_in_state'),
            'runtime_pm': {}}
    for p in sorted((SYS / 'devices').rglob('runtime_status')):
        # Do not follow symlinks into devices, or cause any device MMIO reads.
        data['runtime_pm'][str(p.parent.parent)] = {
            key: read(p.parent / key) for key in (
                'control', 'runtime_status', 'runtime_usage',
                'runtime_active_time', 'runtime_suspended_time')}
    (out / name).write_text(json.dumps(data, indent=2) + '\n')


@contextmanager
def backlight(off):
    saved = []
    try:
        if off:
            devices = sorted((SYS / 'class/backlight').glob('*/brightness'))
            if not devices:
                raise RuntimeError('No backlight brightness control')
            for p in devices:
                value = read(p)
                if not isinstance(value, int):
                    raise RuntimeError(f'Cannot save {p}')
                saved.append((p, value))
                p.write_text('0\n')
                if read(p) != 0:
                    raise RuntimeError(f'Backlight did not accept zero: {p}')
        yield
    finally:
        for p, value in reversed(saved):
            p.write_text(f'{value}\n')


def mean_integrated(rows, getter):
    area = duration = 0.0
    for left, right in zip(rows, rows[1:]):
        a, b = getter(left), getter(right)
        if a is None or b is None:
            continue
        dt = right['monotonic_s'] - left['monotonic_s']
        if dt > 0:
            area += (a + b) * 0.5 * dt
            duration += dt
    return {'mean': area / duration if duration else None,
            'integral_seconds': area, 'valid_duration_s': duration}


def summarize(rows, battery):
    prefix = str(battery) + '/'
    def gauge(row, key):
        v = row['values'].get(prefix + key)
        return v if isinstance(v, int) else None

    def discharge(row, power=False):
        if row['values'].get(prefix + 'status') != 'Discharging':
            return None
        current = gauge(row, 'current_now')
        volts = gauge(row, 'voltage_now')
        if current is None or (power and volts is None):
            return None
        return abs(current) * volts / 1e6 if power else abs(current)

    start, end = rows[0], rows[-1]
    elapsed = end['monotonic_s'] - start['monotonic_s']
    residency = {}
    for path, value in end['values'].items():
        if '/cpuidle/state' not in path or not path.endswith('/time'):
            continue
        old = start['values'].get(path)
        if not isinstance(value, int) or not isinstance(old, int):
            continue
        base = path.rsplit('/', 1)[0] + '/'
        valid = value >= old
        usage0 = start['values'].get(base + 'usage')
        usage1 = end['values'].get(base + 'usage')
        residency[base] = {
            'name': end['values'].get(base + 'name'),
            'time_us': value - old if valid else None,
            'wall_percent': (value - old) / (elapsed * 1e4) if valid and elapsed else None,
            'entries': usage1 - usage0 if isinstance(usage0, int) and isinstance(usage1, int) and usage1 >= usage0 else None,
            'counter_reset': not valid,
        }
    cpu_idle = {}
    for cpu, values in end['cpu_ticks'].items():
        old = start['cpu_ticks'].get(cpu)
        if old is None or len(old) != len(values):
            continue
        delta = [v - o for v, o in zip(values[:8], old[:8])]
        # guest and guest_nice are already included in user and nice.
        if all(v >= 0 for v in delta) and sum(delta) and len(delta) >= 5:
            cpu_idle[cpu] = {'idle_percent': delta[3] * 100 / sum(delta),
                             'iowait_percent': delta[4] * 100 / sum(delta)}
    raw_current = mean_integrated(rows, lambda row: gauge(row, 'current_now'))
    discharge_current = mean_integrated(rows, discharge)
    power = mean_integrated(rows, lambda row: discharge(row, power=True))
    statuses = sorted({str(row['values'].get(prefix + 'status')) for row in rows})
    online = sorted({p for row in rows for p, v in row['values'].items()
                     if '/power_supply/' in p and p.endswith('/online') and v == 1})
    pairs = {(gauge(row, 'current_now'), gauge(row, 'voltage_now')) for row in rows}
    valid_currents = [gauge(row, 'current_now') for row in rows if gauge(row, 'current_now') is not None]
    return {
        'elapsed_s': elapsed, 'samples': len(rows), 'battery': str(battery),
        'statuses': statuses, 'supplies_online': online,
        'current_now_uA_signed': raw_current,
        'discharge_current_uA_magnitude': discharge_current,
        'discharge_power_uW_magnitude': power,
        'discharge_energy_mWh': power['integral_seconds'] / 3600000,
        'median_current_now_uA_signed': statistics.median(valid_currents) if valid_currents else None,
        'unique_current_voltage_pairs': len(pairs),
        'battery_comparison_eligible': statuses == ['Discharging'] and not online and power['valid_duration_s'] >= elapsed * 0.95 and elapsed > 0,
        'cpuidle': residency, 'proc_stat': cpu_idle,
        'notes': [
            'Residency is software time, not proof of hardware retention.',
            'Repeated values may be cached; unique pairs do not establish gauge update cadence.',
            'Screen-off can mean backlight-only; firmware scanout remains powered.',
            'Eligibility cannot detect unreported external power; control USB, brightness, workload and temperature.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='new directory on local storage')
    parser.add_argument('--mode', choices=['desktop', 'screen-off'], required=True)
    parser.add_argument('--duration', type=float, default=300)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--settle', type=float, default=30)
    parser.add_argument('--battery', type=Path)
    parser.add_argument('--backlight-off', action='store_true', help='save/zero/restore brightness; requires screen-off mode')
    args = parser.parse_args()
    if not all(math.isfinite(v) for v in (args.duration, args.interval, args.settle)):
        parser.error('timings must be finite')
    if not (0 < args.duration <= 86400 and 0 < args.interval <= args.duration and 0 <= args.settle <= 3600):
        parser.error('duration 0..86400, interval 0..duration, settle 0..3600')
    if args.backlight_off and args.mode != 'screen-off':
        parser.error('--backlight-off requires --mode screen-off')
    candidates = [p for p in (SYS / 'class/power_supply').glob('*')
                  if read(p / 'type') == 'Battery' and (p / 'current_now').exists()]
    if args.battery is None and len(candidates) != 1:
        parser.error('select --battery: expected one Battery with current_now')
    battery = (args.battery or candidates[0]).absolute()
    if battery.parent != SYS / 'class/power_supply':
        parser.error('--battery must name /sys/class/power_supply/<supply>')
    if read(battery / 'type') != 'Battery' or not (battery / 'current_now').exists():
        parser.error('battery must expose type=Battery and current_now')
    args.output.mkdir(mode=0o700, parents=False, exist_ok=False)
    (args.output / 'metadata.json').write_text(json.dumps({
        'mode': args.mode, 'duration_s': args.duration, 'interval_s': args.interval,
        'settle_s': args.settle, 'backlight_off': args.backlight_off,
        'battery': str(battery), 'kernel': read(PROC / 'version'),
    }, indent=2) + '\n')
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    rows = []
    was_interrupted = False
    with backlight(args.backlight_off):
        try:
            time.sleep(args.settle)
            endpoints(args.output, 'before.json')
            with (args.output / 'samples.jsonl').open('x') as stream:
                deadline = time.monotonic() + args.duration
                next_sample = time.monotonic()
                while True:
                    row = snapshot()
                    rows.append(row)
                    stream.write(json.dumps(row) + '\n')
                    stream.flush()
                    now = time.monotonic()
                    if now >= deadline:
                        break
                    next_sample += args.interval
                    # Skip missed sample slots instead of taking bursts.
                    if next_sample < now:
                        next_sample = now + args.interval
                    time.sleep(max(0, min(next_sample, deadline) - time.monotonic()))
        except KeyboardInterrupt:
            was_interrupted = True
        finally:
            endpoints(args.output, 'after.json')
            if rows:
                summary = summarize(rows, battery)
                summary['interrupted'] = was_interrupted
                (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
                print(json.dumps(summary, indent=2))
    return 130 if was_interrupted else 0


if __name__ == '__main__':
    raise SystemExit(main())
