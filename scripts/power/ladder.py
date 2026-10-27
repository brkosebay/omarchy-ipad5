#!/usr/bin/env python3
"""Orchestrator-only A9 thermal validation and RAM-only clock trials on Linux.

Writes only the guarded kernel interface, cpufreq policy and our watchdog FD.
No SSH, devmem, PMIC, NVRAM, partition, or boot-firmware access.
"""
import argparse
import array
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import a9_thermal

PLL_MAX_KHZ = 511 * 24000
STEPS = (1848000, 1896000, 1944000, 1992000, 2040000, 2088000, 2160000,
         *range(2208000, PLL_MAX_KHZ, 48000), PLL_MAX_KHZ)
ALL_NAMES = a9_thermal.ALL_NAMES
NAMES = a9_thermal.NAMES
PAYLOAD = bytes(range(256)) * 4096
GOLDEN = 'fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83'
DAC_STEP_UV = 3125
CEILING_UV = 50000
CLOCK_PROFILE = 's8003-r6-shared-slot'
BOOST_STATE = 15
DELIVERY_TIMEOUT_S = 10


class ClockFailure(RuntimeError):
    """Verified work failed while sensors and the kernel guard were healthy."""


def emit(event, **fields):
    print(json.dumps({'event': event, 'monotonic_ns': time.monotonic_ns(),
                      'epoch_ns': time.time_ns(), **fields}), flush=True)


def read(path):
    return path.read_text().strip()


def fields(line):
    return dict(item.split('=', 1) for item in line.split() if '=' in item)


def status(root):
    lines = read(root / 'kernel/debug/a9_tuning/status').splitlines()
    result = fields(lines[0])
    result.update(fields(lines[1]))
    return result


def voltage_handoff(state):
    if (state.get('format') != '3' or state.get('handoff_policy') != 's8003-r4' or
            any(int(state.get(key, '-1'), 16) != expected for key, expected in (
                ('voltage_ctl', 0), ('voltage_ctl_boot', 0),
                ('voltage_routes', 0x1782), ('voltage_routes_boot', 0x1782))) or
            state.get('voltage_handoff_ok') != '1'):
        raise RuntimeError(f'round-4 stock voltage handoff mismatch: {state}')


def voltage_readback(root, target, delta_uv):
    raw = read(root / 'kernel/debug/a9_tuning/status')
    lines = raw.splitlines()
    state = fields(lines[0])
    state.update(fields(lines[1]))
    if state.get('format') != '3' or state.get('dac_step_uv') != str(DAC_STEP_UV):
        raise RuntimeError('round-4 handoff/DAC/readback interface required')
    if state.get('clock_profile') != CLOCK_PROFILE or state.get('boost_state') != str(BOOST_STATE):
        raise RuntimeError('round-6 shared-slot clock mapping required; use matching kernel and DTB')
    if int(state['ceiling_delta_uv']) != CEILING_UV or int(state['delta_uv']) != delta_uv:
        raise RuntimeError('voltage ceiling/configuration mismatch')
    voltage_handoff(state)
    rows = {int(r['state']): r for r in map(fields, lines[2:])}
    index = 7 if target == 1800000 else BOOST_STATE if target == STEPS[0] else min(STEPS.index(target) + 8, 14)
    row, stock = rows[index], rows[7]
    base1, base2 = int(stock['boot_psinfo1'], 16), int(stock['boot_psinfo2'], 16)
    mask1, mask2 = 255 << 56, 255 << 32
    if target == 1848000 and delta_uv:
        raise RuntimeError('1848 MHz stock-clone offset is forbidden')
    if target > 1800000:
        expected1 = (base1 & ~(511 << 4 | mask1)) | (target // 24000) << 4
        expected1 |= ((base1 >> 56) + delta_uv // DAC_STEP_UV) << 56
        expected2 = (base2 & ~mask2) | (((base2 >> 32 & 255) + delta_uv // DAC_STEP_UV) << 32)
    else:
        if delta_uv:
            raise RuntimeError('stock/boost offset is forbidden')
        expected1, expected2 = int(row['boot_psinfo1'], 16), int(row['boot_psinfo2'], 16)
    if (int(row['psinfo1'], 16), int(row['psinfo2'], 16)) != (expected1, expected2):
        raise RuntimeError('PSINFO voltage/PLL readback mismatch')
    core, aux = expected1 >> 56, expected2 >> 32 & 255
    if (int(row['core_code']) != core or int(row['aux_code']) != aux or
            int(row['core_nominal_uv']) != 450000 + DAC_STEP_UV * core or
            int(row['aux_nominal_uv']) != 600000 + DAC_STEP_UV * aux):
        raise RuntimeError('nominal DAC conversion mismatch')
    return {'state': index, **row, 'requested_delta_uv': delta_uv,
            'measurement': 'PSINFO table readback; nominal DAC target, not ADC'}


def boot_id():
    return read(Path('/proc/sys/kernel/random/boot_id'))


def control(root, command):
    try:
        with (root / 'kernel/debug/a9_tuning/control').open('w') as stream:
            stream.write(command + '\n')
    except OSError as exc:
        # Keep the rejecting kernel check visible in externally streamed JSONL.
        emit('control-error', command=command, errno=exc.errno, guard=status(root))
        raise


def sensors(root):
    return a9_thermal.sensors(root, reader=read)


def telemetry(root):
    temps, raw = sensors(root)
    state = status(root)
    gauge, gauge_errors, policy = {}, {}, {}
    for name in ('scaling_governor', 'scaling_min_freq', 'scaling_max_freq',
                 'cpuinfo_max_freq', 'scaling_cur_freq', 'cpuinfo_cur_freq', 'boost'):
        try:
            policy[name] = read(root / 'devices/system/cpu/cpufreq/policy0' / name)
        except OSError as exc:
            policy[name] = str(exc)
    for supply in sorted((root / 'class/power_supply').glob('*')):
        for name in ('current_now', 'voltage_now', 'temp', 'status', 'capacity'):
            path = supply / name
            try:
                gauge[str(path.relative_to(root))] = read(path)
            except OSError as exc:
                gauge_errors[str(path.relative_to(root))] = str(exc)
    return {'temperatures_millic': temps, 'thermal_raw': raw,
            'required_sensors': sorted(NAMES),
            'excluded_sensors': sorted(ALL_NAMES - NAMES),
            'thermal_profile': a9_thermal.profile_from_raw(raw),
            'guard': state, 'policy': policy, 'gauge': gauge, 'gauge_errors': gauge_errors,
            'actual_khz': int(state['actual_khz']),
            'units': {'temperature': 'milliC', 'clock': 'kHz',
                      'gauge_current_now': 'uA; battery driver sign, not CPU rail current'}}


def thermal_check(root, seconds):
    if not 0 < seconds < float('inf'):
        raise ValueError('thermal check must have positive duration')
    ranges = {name: [150000, -40000] for name in NAMES}
    end = time.monotonic() + seconds
    samples = 0
    while time.monotonic() < end:
        sample = telemetry(root)
        if not 0 < sample['actual_khz'] <= 1800000 or sample['guard']['armed'] != '0':
            raise RuntimeError('thermal validation requires unarmed stock clocks')
        for name, temp in sample['temperatures_millic'].items():
            ranges[name] = [min(ranges[name][0], temp), max(ranges[name][1], temp)]
        emit('thermal-sample', **sample)
        samples += 1
        time.sleep(0.25)
    emit('thermal-check-pass', samples=samples, ranges_millic=ranges,
         limitation='protocol/plausibility check; compare with iOS and stock load before clock trials')


def ensure_stock(root):
    state = status(root)
    if (state['armed'] != '0' or int(state['dirty'], 16) or
            int(state['revert_error']) or not 0 < int(state['actual_khz']) <= 1800000 or
            int(state.get('configured', '0'), 16) or int(state.get('delta_uv', '0'))):
        raise RuntimeError(f'stock revert is not confirmed: {state}')
    return state


def guard_check(root):
    sensors(root)
    voltage_handoff(status(root))
    ensure_stock(root)
    try:
        control(root, 'arm')
        control(root, 'revert')
        emit('manual-revert-pass', guard=ensure_stock(root))
        control(root, 'arm')
        # Miss the heartbeat at stock clocks, checking sensors throughout.
        end = time.monotonic() + 3.0
        while time.monotonic() < end:
            sensors(root)
            voltage_handoff(status(root))
            time.sleep(0.1)
        state = ensure_stock(root)
        if state['latched'] != '1' or state['reason'] != 'heartbeat-timeout':
            raise RuntimeError(f'heartbeat test did not latch: {state}')
        emit('heartbeat-revert-pass', guard=state)
    finally:
        control(root, 'revert')
        emit('stock-restored', guard=ensure_stock(root))


class Watchdog:
    """Own one watchdog FD; magic-close it on normal and handled-error exits."""
    def __init__(self, path, root):
        nowayout = root / 'class/watchdog' / path.name / 'nowayout'
        if read(nowayout) != '0':
            raise RuntimeError('watchdog must support stopping; nowayout is set')
        if read(nowayout.with_name('identity')) != 'Apple SoC Watchdog':
            raise RuntimeError('Apple hardware watchdog required')
        self.fd = os.open(path, os.O_WRONLY | os.O_CLOEXEC)
        self.timeout = None
        try:
            info = bytearray(40)
            fcntl.ioctl(self.fd, 0x80285700, info, True)  # WDIOC_GETSUPPORT
            if not int.from_bytes(info[:4], sys.byteorder) & 0x100:  # WDIOF_MAGICCLOSE
                raise RuntimeError('watchdog has no magic-close support')
            value = array.array('i', [10])
            fcntl.ioctl(self.fd, 0xc0045706, value, True)  # WDIOC_SETTIMEOUT
            self.timeout = value[0]
            if not 3 <= self.timeout <= 15:
                raise RuntimeError(f'unsupported watchdog timeout: {self.timeout}')
            self.ping()
        except BaseException:
            self.close()
            raise

    def ping(self):
        os.write(self.fd, b'.')

    def close(self):
        if self.fd is not None:
            try:
                os.write(self.fd, b'V')
            finally:
                os.close(self.fd)
                self.fd = None

    def abandon(self):
        # A failed stock revert must retain the hardware reset fallback.
        if self.fd is not None:
            os.close(self.fd)  # No magic-close byte: the watchdog stays active.
            self.fd = None


def watchdog_check(root, path):
    """An explicit stock-only destructive test; success is an observed reset."""
    ensure_stock(root)
    sensors(root)
    watchdog = Watchdog(path, root)
    try:
        ensure_stock(root)
        sensors(root)
        emit('watchdog-reset-start', boot_id=boot_id(), timeout_s=watchdog.timeout,
             action='stop feeding owned Apple watchdog at confirmed stock clocks')
    except BaseException:
        watchdog.close()
        raise
    watchdog.abandon()
    time.sleep(watchdog.timeout + 5)
    raise RuntimeError('hardware watchdog did not reset within timeout + 5 s; no tuning allowed')


def stop_child(child):
    if child is None or child.poll() is not None:
        return
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=3)


def trial(root, policy, target, seconds, logdir, watchdog, delta_uv=0, *, cores=2):
    if target not in (*STEPS, 1800000) or not 0 < seconds < float('inf') or cores not in (1, 2):
        raise ValueError('invalid target/duration/cores')
    if not 0 <= delta_uv <= CEILING_UV or delta_uv % DAC_STEP_UV:
        raise ValueError('offset must be an exact 3125 uV step within +50000 uV')
    if target <= 1848000 and delta_uv:
        raise ValueError('stock/boost voltage must remain unchanged')
    child = None
    seen = 0
    samples = 0
    started = False
    with (logdir / f'{target}-{delta_uv}-stress.log').open('x') as log:
        try:
            ensure_stock(root)
            selected = a9_thermal.profile(root, reader=read)
            if max(sensors(root)[0].values()) >= selected['arm_millic']:
                raise RuntimeError(f"cool below {selected['arm_millic']} milliC before arming")
            control(root, 'arm')
            started = True
            if target > 1800000:
                control(root, f'configure {target} {delta_uv}')
            voltage = voltage_readback(root, target, delta_uv)
            # Kernel guard and hardware max-load restrictions remain effective.
            (root / 'devices/system/cpu/cpufreq/boost').write_text('1\n')
            (policy / 'scaling_max_freq').write_text(f'{target}\n')
            (policy / 'scaling_governor').write_text('performance\n')
            control(root, 'heartbeat')
            command = ['stress-ng', '--cpu', str(cores),
                       '--cpu-method', 'matrixprod', '--verify', '--timeout',
                       f'{seconds:g}s', '--metrics-brief']
            last_delivered = time.monotonic()
            end = last_delivered + seconds
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
            emit('trial-start', target_khz=target, delta_uv=delta_uv, seconds=seconds, cores=cores,
                 command=command, watchdog_timeout_s=watchdog.timeout,
                 boot_id=boot_id(), voltage_readback=voltage, thermal_profile=selected)
            while True:
                now = time.monotonic()
                sample = telemetry(root)
                state = sample['guard']
                if state['armed'] != '1' or state['latched'] != '0':
                    raise RuntimeError(f'kernel guard tripped: {state}')
                voltage = voltage_readback(root, target, delta_uv)
                if not 0 < sample['actual_khz'] <= target:
                    raise RuntimeError('out-of-range actual clock')
                seen += sample['actual_khz'] == target
                samples += 1
                if sample['actual_khz'] == target:
                    last_delivered = now
                elif now - last_delivered >= DELIVERY_TIMEOUT_S:
                    emit('delivery-timeout', target_khz=target, guard=state,
                         policy=sample.get('policy', {}), actual_khz=sample['actual_khz'])
                    reason = 'was never delivered' if not seen else 'stopped being delivered'
                    raise RuntimeError(f'target {reason} within {DELIVERY_TIMEOUT_S} s; no voltage retry')
                if child.poll() is not None:
                    if child.returncode in (2, -4, -7, -8, -11):
                        if not seen:
                            raise RuntimeError('workload failed before target was delivered')
                        raise ClockFailure(f'stress-ng verification/signal failure: {child.returncode}')
                    if child.returncode or now < end - 1:
                        raise RuntimeError(f'stress-ng failed/exited early: {child.returncode}')
                    break
                if now > end + 10:
                    raise RuntimeError('stress-ng did not finish within its timeout')
                if hashlib.sha256(PAYLOAD).hexdigest() != GOLDEN:
                    if not seen:
                        raise RuntimeError('checksum failed before target was delivered')
                    raise ClockFailure('SHA256 correctness failure')
                emit('trial-sample', target_khz=target, delta_uv=delta_uv,
                     voltage_readback=voltage, **sample)
                # Feed both watchdogs only after all independent checks pass.
                control(root, 'heartbeat')
                watchdog.ping()
                time.sleep(0.25)
            if not seen:
                raise RuntimeError('target was never delivered; requested clock is not a result')
            if seen < samples * 0.8:
                raise RuntimeError('target delivered in less than 80% of samples')
            emit('trial-pass', target_khz=target, delta_uv=delta_uv,
                 target_samples=seen, samples=samples, cores=cores, seconds=seconds,
                 limitation='state-correlated frequency; independent cycle timing still required')
        finally:
            # Revert before waiting for the workload to exit.
            try:
                if started:
                    control(root, 'revert')
            finally:
                stop_child(child)
            if started:
                emit('stock-restored', guard=ensure_stock(root))


def cooldown(root, watchdog):
    end = time.monotonic() + 120
    selected = a9_thermal.profile(root, reader=read)
    while max(sensors(root)[0].values()) >= selected['cooldown_millic']:
        ensure_stock(root)
        if time.monotonic() > end:
            raise RuntimeError(f"cooldown did not reach below {selected['cooldown_millic']} milliC within two minutes")
        watchdog.ping()
        time.sleep(0.25)


def retry_candidate(path):
    """Explicit orchestrator resume after a reset, never automatic at boot."""
    events = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    starts = [i for i, e in enumerate(events) if e.get('event') == 'trial-start']
    if not starts:
        raise ValueError('retry log contains no started trial')
    start = starts[-1]
    row = events[start]
    if any(e.get('event') in ('trial-pass', 'failure', 'summary', 'stock-restored', 'stock-unconfirmed')
           for e in events[start + 1:]):
        raise ValueError('retry log has a completed/handled trial; no reset evidence')
    if row.get('boot_id') in (None, '', boot_id()):
        raise ValueError('retry requires a new boot ID and the interrupted trial log')
    target, delta = row['target_khz'], row['delta_uv']
    if target not in STEPS[1:] or delta < 0 or delta >= CEILING_UV or delta % DAC_STEP_UV:
        raise ValueError('no permitted voltage retry for that trial')
    return target, delta


def trial_seconds(args):
    seconds = getattr(args, 'seconds', None)
    return seconds if seconds is not None else args.minutes * 60


def trial_with_offsets(args, policy, target, watchdog, start_delta=0):
    for delta in range(start_delta, args.max_delta_uv + 1, args.voltage_step_uv):
        try:
            trial(args.sysfs, policy, target, trial_seconds(args), args.logdir, watchdog, delta,
                  cores=getattr(args, 'cores', 2))
            return delta
        except ClockFailure as exc:
            emit('clock-failure', target_khz=target, delta_uv=delta, error=str(exc))
            # The trial's finally block must already have confirmed restoration.
            ensure_stock(args.sysfs)
            if target == STEPS[0] or delta + args.voltage_step_uv > args.max_delta_uv:
                raise
            cooldown(args.sysfs, watchdog)
            # A stock correctness failure is an abort, never a voltage argument.
            control_dir = args.logdir / f'stock-control-{target}-{delta}'
            control_dir.mkdir()
            trial(args.sysfs, policy, 1800000, args.control_seconds, control_dir, watchdog,
                  cores=getattr(args, 'cores', 2))
            cooldown(args.sysfs, watchdog)
    raise RuntimeError('no permitted voltage attempt')


def run_trials(args):
    if not shutil.which('stress-ng'):
        raise RuntimeError('stress-ng is required; install it through the orchestrator')
    if not 0 < trial_seconds(args) < float('inf'):
        raise ValueError('trial duration must be positive and finite')
    if getattr(args, 'from_khz', STEPS[0]) > getattr(args, 'through_khz', STEPS[-1]):
        raise ValueError('from-khz must not exceed through-khz')
    if (not 0 <= args.max_delta_uv <= CEILING_UV or args.max_delta_uv % args.voltage_step_uv
            or args.control_seconds < 10):
        raise ValueError('invalid voltage ceiling/step or stock-control duration')
    retry = retry_candidate(args.retry_failure) if args.retry_failure else None
    if retry and (args.mode != 'ladder' or not getattr(args, 'from_khz', STEPS[0]) <= retry[0] <= args.through_khz or
                  retry[1] + args.voltage_step_uv > args.max_delta_uv):
        raise ValueError('retry is outside this ladder/ceiling')
    args.logdir.mkdir(parents=True, exist_ok=False)
    policy = args.sysfs / 'devices/system/cpu/cpufreq/policy0'
    if read(policy / 'scaling_driver') != 'apple-cpufreq':
        raise RuntimeError('expected apple-cpufreq policy0')
    voltage_handoff(status(args.sysfs))
    if status(args.sysfs)['enable'] != '1':
        raise RuntimeError('boot with apple_soc_cpufreq.a9_tuning=1')
    saved = {name: read(policy / name) for name in
             ('scaling_governor', 'scaling_min_freq', 'scaling_max_freq')}
    stable, stable_by_delta, attempts = [], {'0': None}, []
    watchdog = Watchdog(args.watchdog, args.sysfs)
    confirmed_stock = False
    try:
        (policy / 'scaling_min_freq').write_text('300000\n')
        targets = (tuple(t for t in STEPS
                         if getattr(args, 'from_khz', STEPS[0]) <= t <= args.through_khz)
                   if args.mode == 'ladder' else STEPS[:1])
        if retry:
            targets = tuple(t for t in targets if t >= retry[0])
            control_dir = args.logdir / 'stock-control-after-reset'
            control_dir.mkdir()
            trial(args.sysfs, policy, 1800000, args.control_seconds, control_dir, watchdog,
                  cores=getattr(args, 'cores', 2))
            cooldown(args.sysfs, watchdog)
        for target in targets:
            # Each new clock starts at stock voltage, regardless of prior rungs.
            first_delta = retry[1] + args.voltage_step_uv if retry and target == retry[0] else 0
            delta = trial_with_offsets(args, policy, target, watchdog, first_delta)
            stable.append(target)
            stable_by_delta[str(delta)] = max(stable_by_delta.get(str(delta)) or 0, target)
            attempts.append({'target_khz': target, 'delta_uv': delta})
            # Start each step with stock restored and let temperatures settle.
            cooldown(args.sysfs, watchdog)
    finally:
        try:
            control(args.sysfs, 'revert')
            (args.sysfs / 'devices/system/cpu/cpufreq/boost').write_text('0\n')
            (policy / 'scaling_min_freq').write_text(saved['scaling_min_freq'] + '\n')
            (policy / 'scaling_max_freq').write_text(str(min(int(saved['scaling_max_freq']), 1800000)) + '\n')
            (policy / 'scaling_governor').write_text(saved['scaling_governor'] + '\n')
            ensure_stock(args.sysfs)
            confirmed_stock = True
        finally:
            if confirmed_stock:
                watchdog.close()
            else:
                watchdog.abandon()
                emit('stock-unconfirmed', fallback='hardware-watchdog-reset')
            result = {'cores': getattr(args, 'cores', 2), 'seconds': trial_seconds(args),
                      'max_stable_khz_by_delta_uv': stable_by_delta,
                      'passed_khz': stable, 'passes': attempts,
                      'voltage_increases': 'only after correctness failure at lower voltage',
                      'stock_nominal_uv': {'core': 812500, 'buck7': 915625},
                      'measured_rail_uv': None, 'hard_ceiling_delta_uv': CEILING_UV}
            (args.logdir / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            emit('summary', **result)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('thermal-check', 'guard-check', 'watchdog-check', 'boost', 'ladder'))
    duration = p.add_mutually_exclusive_group()
    duration.add_argument('--seconds', type=float, help='trial seconds; thermal-check defaults to 30')
    duration.add_argument('--minutes', type=float, default=5, help='trial minutes (default 5)')
    p.add_argument('--cores', type=int, choices=(1, 2), default=2)
    p.add_argument('--from-khz', type=int, choices=STEPS, default=STEPS[0],
                   help='first ladder rung; set equal to through-khz to confirm one clock')
    p.add_argument('--through-khz', type=int, choices=STEPS, default=2160000)
    p.add_argument('--logdir', type=Path, default=Path('a9-trial-' + time.strftime('%Y%m%d-%H%M%S')))
    p.add_argument('--watchdog', type=Path, default=Path('/dev/watchdog0'))
    p.add_argument('--max-delta-uv', type=int, default=CEILING_UV)
    p.add_argument('--voltage-step-uv', type=int, choices=(3125, 6250, 12500), default=6250)
    p.add_argument('--control-seconds', type=int, default=30)
    p.add_argument('--retry-failure', type=Path,
                   help='explicit retry of interrupted JSONL trial after watchdog/reset; requires new boot ID')
    p.add_argument('--sysfs', type=Path, default=Path('/sys'))
    return p.parse_args(argv)


def main():
    args = parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise SystemExit('Run as root on the iPad Linux system, through the orchestrator only')
    with Path('/run/a9-tuning.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, lambda signum, frame: (_ for _ in ()).throw(InterruptedError(signum)))
        if args.mode == 'thermal-check':
            thermal_check(args.sysfs, args.seconds if args.seconds is not None else 30)
        elif args.mode == 'guard-check':
            guard_check(args.sysfs)
        elif args.mode == 'watchdog-check':
            watchdog_check(args.sysfs, args.watchdog)
        else:
            run_trials(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, InterruptedError) as exc:
        emit('failure', error=str(exc))
        raise SystemExit(1)
