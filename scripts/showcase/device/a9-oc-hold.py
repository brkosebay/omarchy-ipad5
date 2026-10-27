#!/usr/bin/env python3
"""Hold the A9 at a guarded 2160 MHz for the showcase (root, on the iPad).

Copied next to scripts/power/{ladder,a9_thermal}.py by scripts/showcase/bring-up.sh
and run there as the transient unit linux-a9-oc-hold. It drives the round-5/6
kernel guard the way ladder.py's trial and bench-ab.py do: stock confirmed,
the sustained 90/95/100 C profile from the boot arguments, every trusted
sensor below the arm limit, `arm`, `configure 2160000 0` (stock voltage), the
PSINFO/DAC readback, boost on, scaling_max_freq 2160000, then one heartbeat
every --interval seconds while every check passes.

The kernel guard stays in charge: it reverts to stock at 90 C (passive), 95 C
(hot), on a sensor or voltage-mode fault, and 2 s after the last heartbeat, so
a dead or starved daemon means stock clocks. This daemon never re-arms after a
revert: it logs the kernel's reason and exits 3. Running bring-up again re-arms
once the SoC is below the arm limit. No voltage offset, no watchdog device, no
thermal trip writes; the governor is left alone unless --governor is given.

  a9-oc-hold.py preflight      checks only; JSON; exit 0 when hold may start
  a9-oc-hold.py hold           the daemon
  a9-oc-hold.py status         guard, policy and daemon state; JSON
  a9-oc-hold.py load-test      two busy workers for --seconds; actual clock from
                               the guard status; exit 0 when >= 80 % of samples
                               ran at the target

Exit codes of hold: 0 stopped by a signal (reverted, stock confirmed),
1 a check failed (reverted by us), 2 refused to start, 3 the kernel guard
reverted (its reason is in the log and the state file).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import a9_thermal  # noqa: E402
import ladder  # noqa: E402

KHZ = 2160000
STOCK_KHZ = 1800000
TUNING = ('apple_soc_cpufreq.a9_tuning=1', 'apple_soc_cpufreq.a9_passive_millic=90000')
SUSTAINED = dict(profile='sustained', profile_tuning=1, passive_millic=90000, hot_millic=95000,
                 critical_millic=100000, arm_millic=85000, cooldown_millic=80000)
STATE = Path(os.environ.get('A9_OC_HOLD_STATE', '/run/linux-a9/oc-hold.json'))
EXIT_STOPPED, EXIT_FAILED, EXIT_REFUSED, EXIT_TRIPPED = 0, 1, 2, 3


def policy(root):
    return root / 'devices/system/cpu/cpufreq/policy0'


def configured_mask(khz):
    return f'{1 << (min(ladder.STEPS.index(khz) + 8, 14)):x}'


def write_state(**fields):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_name(STATE.name + '.tmp')
    tmp.write_text(json.dumps(dict(fields, pid=os.getpid(), time=time.time())) + '\n')
    tmp.replace(STATE)


def read_policy(root):
    out = {}
    for name in ('scaling_driver', 'scaling_governor', 'scaling_max_freq', 'cpuinfo_max_freq',
                 'scaling_cur_freq', 'scaling_boost_frequencies'):
        try:
            out[name] = ladder.read(policy(root) / name)
        except OSError:
            out[name] = None
    try:
        out['boost'] = ladder.read(root / 'devices/system/cpu/cpufreq/boost')
    except OSError:
        out['boost'] = None
    return out


def guard_or_empty(root):
    try:
        return ladder.status(root)
    except (OSError, IndexError, ValueError):
        return {}


def held(state, khz):
    return (state.get('armed') == '1' and state.get('latched') == '0' and
            state.get('limit_khz') == str(khz) and state.get('configured') == configured_mask(khz) and
            state.get('delta_uv') == '0')


def preflight(root, proc, khz):
    """Every precondition for arming, or for an already running hold."""
    checks = []

    def check(name, func):
        try:
            detail = func()
            checks.append(dict(check=name, ok=True, detail=detail))
        except (RuntimeError, OSError, KeyError, ValueError, IndexError) as exc:
            checks.append(dict(check=name, ok=False, detail=str(exc)))

    def cmdline():
        words = (proc / 'cmdline').read_text().split()
        missing = [t for t in TUNING if t not in words]
        if missing:
            raise RuntimeError(f'boot arguments missing: {missing}')
        return 'tuning boot arguments present'

    def guard():
        state = ladder.status(root)
        if state.get('enable') != '1':
            raise RuntimeError(f'guard not enabled: enable={state.get("enable")}')
        if state.get('clock_profile') != ladder.CLOCK_PROFILE or state.get('boost_state') != str(ladder.BOOST_STATE):
            raise RuntimeError(f'clock profile {state.get("clock_profile")}; want {ladder.CLOCK_PROFILE}')
        ladder.voltage_handoff(state)
        if held(state, khz):
            return 'already held at the target'
        ladder.ensure_stock(root)
        return f'stock, reason={state.get("reason")}'

    def profile():
        got = a9_thermal.profile(root, reader=ladder.read)
        if got != SUSTAINED:
            raise RuntimeError(f'thermal profile {got}; want {SUSTAINED}')
        return got['profile']

    def temperatures():
        temps = ladder.sensors(root)[0]
        hottest = max(temps.values())
        if not held(ladder.status(root), khz) and hottest >= SUSTAINED['arm_millic']:
            raise RuntimeError(f'hottest {hottest} milliC; cool below {SUSTAINED["arm_millic"]} first')
        return temps

    def table():
        boost = ladder.read(policy(root) / 'scaling_boost_frequencies').split()
        if boost != [str(khz)]:
            raise RuntimeError(f'turbo table {boost}; want only {khz} (showcase DTB)')
        return boost

    check('cmdline', cmdline)
    check('guard', guard)
    check('profile', profile)
    check('temperatures', temperatures)
    if (policy(root) / 'scaling_boost_frequencies').exists():
        check('turbo-table', table)
    return dict(ok=all(c['ok'] for c in checks), checks=checks)


class Stop:
    def __init__(self):
        self.event = threading.Event()
        self.signal = None

    def __call__(self, signum, frame):
        self.signal = signal.Signals(signum).name
        self.event.set()


def hold(root, proc, khz, interval, governor, stop):
    pre = preflight(root, proc, khz)
    if not pre['ok'] or held(guard_or_empty(root), khz):
        ladder.emit('refused', preflight=pre, reason='preflight failed' if not pre['ok'] else 'guard already held')
        write_state(state='refused', khz=khz, preflight=pre)
        return EXIT_REFUSED
    boost = root / 'devices/system/cpu/cpufreq/boost'
    saved = dict(max=ladder.read(policy(root) / 'scaling_max_freq'),
                 governor=ladder.read(policy(root) / 'scaling_governor'), boost=ladder.read(boost))
    started, since, ticks, result, why = False, time.time(), 0, EXIT_FAILED, ''
    try:
        ladder.ensure_stock(root)
        ladder.control(root, 'arm')
        started = True
        ladder.control(root, f'configure {khz} 0')
        voltage = ladder.voltage_readback(root, khz, 0)
        boost.write_text('1\n')
        (policy(root) / 'scaling_max_freq').write_text(f'{khz}\n')
        if governor:
            (policy(root) / 'scaling_governor').write_text(f'{governor}\n')
        ladder.control(root, 'heartbeat')
        ladder.emit('hold-start', khz=khz, delta_uv=0, interval_s=interval, governor=governor or saved['governor'],
                    voltage_readback=voltage, thermal_profile=SUSTAINED)
        while not stop.event.is_set():
            # Each tick: the guard still holds the target, every trusted sensor
            # is valid and below passive, the PSINFO words are the delta-0 ones
            # and the clock is in range. Only then the heartbeat.
            state = ladder.status(root)
            if not held(state, khz):
                raise RuntimeError(f'guard no longer holds {khz}')
            temps = ladder.sensors(root)[0]
            ladder.voltage_readback(root, khz, 0)
            actual = int(state['actual_khz'])
            if not 0 < actual <= khz:
                raise RuntimeError(f'actual clock {actual} out of range')
            ladder.control(root, 'heartbeat')
            ticks += 1
            write_state(state='holding', khz=khz, since=since, ticks=ticks, actual_khz=actual,
                        hottest_millic=max(temps.values()), reason=state.get('reason'))
            stop.event.wait(interval)
        result, why = EXIT_STOPPED, f'signal {stop.signal}'
        return result
    except (RuntimeError, OSError, KeyError, ValueError) as exc:
        why = f'{type(exc).__name__}: {exc}'
        state = guard_or_empty(root)
        if started and state.get('armed') == '0' and state.get('latched') == '1':
            # The kernel reverted first (90 C passive, a fault, a late beat):
            # keep its reason; never re-arm from here.
            result, why = EXIT_TRIPPED, state.get('reason', '?')
            ladder.emit('guard-reverted', reason=why, detail=f'{type(exc).__name__}: {exc}', guard=state)
            return result
        ladder.emit('hold-failed', error=why, guard=state)
        return EXIT_FAILED
    finally:
        # Our revert only when the kernel has not already reverted, so its
        # trip reason stays readable in the guard status.
        if started and result != EXIT_TRIPPED:
            try:
                ladder.control(root, 'revert')
            except OSError:
                pass
        for path, value in ((policy(root) / 'scaling_max_freq', str(min(int(saved['max']), STOCK_KHZ))),
                            (policy(root) / 'scaling_governor', saved['governor']),
                            (boost, saved['boost'])):
            try:
                path.write_text(value + '\n')
            except OSError:
                pass
        final = guard_or_empty(root)
        try:
            ladder.ensure_stock(root)
            restored = True
        except (RuntimeError, OSError, KeyError, ValueError):
            restored = False
        ladder.emit('hold-end', result=result, why=why, stock_restored=restored, guard=final)
        write_state(state={EXIT_STOPPED: 'stopped', EXIT_TRIPPED: 'tripped'}.get(result, 'failed'),
                    khz=khz, since=since, ticks=ticks, why=why, reason=final.get('reason'),
                    stock_restored=restored)


def load_test(root, khz, seconds, interval=0.25):
    state = ladder.status(root)
    if not held(state, khz):
        print(json.dumps(dict(event='load-test', ok=False, error='guard is not holding the target', guard=state)))
        return 1
    if shutil.which('stress-ng'):
        command = [['stress-ng', '--cpu', '2', '--cpu-method', 'matrixprod', '--timeout', f'{seconds + 2:g}s']]
    else:
        command = [[sys.executable, '-c', 'while True: pass']] * 2
    workers = [subprocess.Popen(c, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                start_new_session=True) for c in command]
    samples, hottest, error = [], 0, None
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = ladder.status(root)
            if state['armed'] != '1' or state['latched'] != '0':
                error = f'guard reverted during the load: {state.get("reason")}'
                break
            samples.append(int(state['actual_khz']))
            hottest = max(hottest, max(ladder.sensors(root)[0].values()))
            time.sleep(interval)
    except (RuntimeError, OSError) as exc:
        error = str(exc)
    finally:
        for worker in workers:
            try:
                os.killpg(worker.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for worker in workers:
            try:
                worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(worker.pid, signal.SIGKILL)
                worker.wait()
    at_target = sum(s == khz for s in samples)
    ok = error is None and bool(samples) and at_target >= 0.8 * len(samples)
    print(json.dumps(dict(event='load-test', ok=ok, target_khz=khz, samples=len(samples), at_target=at_target,
                          max_khz=max(samples, default=0),
                          median_khz=statistics.median(samples) if samples else 0,
                          hottest_millic=hottest, workers=command[0][0], error=error)))
    return 0 if ok else 1


def status(root):
    try:
        daemon = json.loads(STATE.read_text())
    except (OSError, ValueError):
        daemon = None
    guard = guard_or_empty(root)
    print(json.dumps(dict(guard=guard, policy=read_policy(root), daemon=daemon,
                          held=held(guard, KHZ))))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('preflight', 'hold', 'status', 'load-test'))
    ap.add_argument('--khz', type=int, choices=(KHZ,), default=KHZ)
    ap.add_argument('--interval', type=float, default=0.5, help='heartbeat period (s); the guard allows 2')
    ap.add_argument('--governor', choices=('performance', 'schedutil'), help='default: leave it')
    ap.add_argument('--seconds', type=float, default=8.0, help='load-test duration')
    ap.add_argument('--sysfs', type=Path, default=Path('/sys'))
    ap.add_argument('--proc', type=Path, default=Path('/proc'))
    args = ap.parse_args(argv)
    if not 0.1 <= args.interval <= 1.0 or not 2 <= args.seconds <= 120:
        ap.error('--interval must be 0.1..1.0 s and --seconds 2..120')
    if args.command == 'preflight':
        result = preflight(args.sysfs, args.proc, args.khz)
        print(json.dumps(result))
        return 0 if result['ok'] else 1
    if args.command == 'status':
        return status(args.sysfs)
    if args.command == 'load-test':
        return load_test(args.sysfs, args.khz, args.seconds)
    stop = Stop()
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, stop)
    return hold(args.sysfs, args.proc, args.khz, args.interval, args.governor, stop)


if __name__ == '__main__':
    sys.exit(main())
