#!/usr/bin/env python3
"""Host-only tests of scripts/showcase/device/a9-oc-hold.py against a model of
the a9_tuning guard (arm/configure/heartbeat/revert, kernel trips)."""
import contextlib
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/power'))
spec = importlib.util.spec_from_file_location('oc_hold', ROOT / 'scripts/showcase/device/a9-oc-hold.py')
hold = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hold)
ladder = hold.ladder
CMDLINE = 'earlycon omarchy.root=nvme apple_soc_cpufreq.a9_tuning=1 apple_soc_cpufreq.a9_passive_millic=90000\n'


class Guard:
    """The kernel guard as the daemon sees it through debugfs."""

    def __init__(self):
        self.state = dict(format='3', enable='1', armed='0', latched='1', reason='user-revert',
                          limit_khz='1800000', configured='0', dirty='0', revert_error='0', delta_uv='0',
                          clock_profile=ladder.CLOCK_PROFILE, boost_state='15', actual_khz='1800000',
                          voltage_ctl='00000000', voltage_routes='00001782', voltage_ctl_boot='00000000',
                          voltage_routes_boot='00001782', voltage_handoff_ok='1', handoff_policy='s8003-r4')
        self.commands = []
        self.temps = {'apple-a9-soc0': 50000, 'apple-a9-soc1': 51000, 'apple-a9-tcc2': 55000}
        self.sensor_error = None

    def status(self, root):
        return dict(self.state)

    def trip(self, reason):
        self.state.update(armed='0', latched='1', reason=reason, limit_khz='1800000', configured='0',
                          actual_khz='1800000')

    def control(self, root, command):
        self.commands.append(command)
        if command == 'arm':
            self.state.update(armed='1', latched='0', reason='armed', limit_khz='1848000')
        elif command.startswith('configure '):
            khz = command.split()[1]
            self.state.update(limit_khz=khz, configured='4000', actual_khz=khz)
        elif command == 'heartbeat':
            if self.state['armed'] != '1':
                raise OSError(errno.EPERM, 'not armed')
        elif command == 'revert':
            self.trip('user-revert')

    def sensors(self, root):
        if self.sensor_error:
            raise RuntimeError(self.sensor_error)
        return dict(self.temps), ''


class Ticks:
    """Stands in for the stop event: runs one test action per heartbeat period."""

    def __init__(self, actions, stop_after):
        self.actions, self.stop_after, self.count, self.stopped = actions, stop_after, 0, False

    def is_set(self):
        return self.stopped

    def wait(self, timeout):
        self.count += 1
        self.actions.get(self.count, lambda: None)()
        if self.count >= self.stop_after:
            self.stopped = True


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / 'sys'
        self.proc = Path(tmp.name) / 'proc'
        self.policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        self.policy.mkdir(parents=True)
        self.proc.mkdir()
        (self.proc / 'cmdline').write_text(CMDLINE)
        for name, value in dict(scaling_driver='apple-cpufreq', scaling_governor='schedutil',
                                scaling_max_freq='1800000', cpuinfo_max_freq='1800000',
                                scaling_boost_frequencies='2160000').items():
            (self.policy / name).write_text(value + '\n')
        (self.root / 'devices/system/cpu/cpufreq/boost').write_text('0\n')
        self.guard = Guard()
        self.readback = mock.Mock(return_value={'state': 14})
        state_file = Path(tmp.name) / 'run/oc-hold.json'
        self.output = io.StringIO()
        for target, name, value in ((ladder, 'status', self.guard.status), (ladder, 'control', self.guard.control),
                                    (ladder, 'sensors', self.guard.sensors),
                                    (ladder, 'voltage_readback', self.readback),
                                    (hold.a9_thermal, 'profile', lambda *a, **k: dict(hold.SUSTAINED)),
                                    (hold, 'STATE', state_file)):
            patch = mock.patch.object(target, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        redirect = contextlib.redirect_stdout(self.output)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)
        self.state_file = state_file

    def run_hold(self, actions=None, stop_after=3, governor=None):
        stop = hold.Stop()
        stop.event = Ticks(actions or {}, stop_after)
        stop.signal = 'SIGTERM'
        return hold.hold(self.root, self.proc, hold.KHZ, 0.5, governor, stop)

    def events(self):
        return [json.loads(line) for line in self.output.getvalue().splitlines() if line.startswith('{')]

    def final(self):
        return json.loads(self.state_file.read_text())

    def assert_policy_restored(self):
        self.assertEqual((self.policy / 'scaling_max_freq').read_text().strip(), '1800000')
        self.assertEqual((self.policy / 'scaling_governor').read_text().strip(), 'schedutil')
        self.assertEqual((self.root / 'devices/system/cpu/cpufreq/boost').read_text().strip(), '0')


class HoldTests(Base):
    def test_configured_mask_is_slot_14(self):
        self.assertEqual(hold.configured_mask(2160000), '4000')

    def test_clean_hold_arms_configures_beats_and_reverts_on_signal(self):
        seen = {}
        actions = {1: lambda: seen.update(boost=(self.root / 'devices/system/cpu/cpufreq/boost').read_text(),
                                          max=(self.policy / 'scaling_max_freq').read_text())}
        self.assertEqual(self.run_hold(actions), hold.EXIT_STOPPED)
        self.assertEqual(seen, dict(boost='1\n', max='2160000\n'))
        self.assertEqual(self.guard.commands[:3], ['arm', 'configure 2160000 0', 'heartbeat'])
        self.assertEqual(self.guard.commands.count('heartbeat'), 4)
        self.assertEqual(self.guard.commands[-1], 'revert')
        self.assertTrue(all(c.args[1:] == (2160000, 0) for c in self.readback.call_args_list))
        self.assert_policy_restored()
        self.assertEqual(self.final()['state'], 'stopped')
        self.assertTrue(self.final()['stock_restored'])

    def test_kernel_trip_keeps_its_reason_and_is_never_rearmed(self):
        result = self.run_hold({2: lambda: self.guard.trip('thermal-passive')}, stop_after=10)
        self.assertEqual(result, hold.EXIT_TRIPPED)
        self.assertEqual(self.guard.commands.count('arm'), 1)
        self.assertNotIn('revert', self.guard.commands)
        self.assertEqual(self.final()['state'], 'tripped')
        self.assertEqual(self.final()['reason'], 'thermal-passive')
        self.assert_policy_restored()

    def test_hot_sensor_failure_with_kernel_revert_is_a_trip(self):
        def heat():
            self.guard.sensor_error = 'apple-a9-tcc2: 90100 outside the trial temperature range'
            self.guard.trip('thermal-passive')
        self.assertEqual(self.run_hold({1: heat}, stop_after=10), hold.EXIT_TRIPPED)
        self.assertNotIn('revert', self.guard.commands)

    def test_sensor_failure_while_armed_reverts(self):
        def fail():
            self.guard.sensor_error = 'invalid/stale sensor'
        self.assertEqual(self.run_hold({1: fail}, stop_after=10), hold.EXIT_FAILED)
        self.assertEqual(self.guard.commands[-1], 'revert')
        self.assertEqual(self.final()['state'], 'failed')
        self.assert_policy_restored()

    def test_readback_mismatch_reverts(self):
        def mismatch():
            self.readback.side_effect = RuntimeError('PSINFO voltage/PLL readback mismatch')
        self.assertEqual(self.run_hold({2: mismatch}, stop_after=10), hold.EXIT_FAILED)
        self.assertEqual(self.guard.commands[-1], 'revert')
        self.assertIn('readback mismatch', self.final()['why'])

    def test_out_of_range_clock_reverts(self):
        def overclock():
            self.guard.state['actual_khz'] = '2208000'
        self.assertEqual(self.run_hold({1: overclock}, stop_after=10), hold.EXIT_FAILED)
        self.assertEqual(self.guard.commands[-1], 'revert')

    def test_performance_governor_is_optional_and_restored(self):
        seen = []
        self.run_hold({1: lambda: seen.append((self.policy / 'scaling_governor').read_text().strip())},
                      governor='performance')
        self.assertEqual(seen, ['performance'])
        self.assert_policy_restored()

    def refused(self):
        self.assertEqual(self.run_hold(), hold.EXIT_REFUSED)
        self.assertEqual(self.guard.commands, [])
        self.assertEqual(self.final()['state'], 'refused')

    def test_refuses_without_tuning_boot_arguments(self):
        (self.proc / 'cmdline').write_text('earlycon omarchy.root=nvme\n')
        self.refused()

    def test_refuses_above_the_arm_limit(self):
        self.guard.temps['apple-a9-tcc2'] = 85000
        self.refused()

    def test_refuses_stock_profile(self):
        with mock.patch.object(hold.a9_thermal, 'profile', return_value=dict(hold.SUSTAINED, profile='stock')):
            self.refused()

    def test_refuses_old_clock_interface(self):
        self.guard.state['clock_profile'] = 's8003-r5-all-load'
        self.refused()

    def test_refuses_dirty_or_armed_guard(self):
        self.guard.state['dirty'] = '4000'
        self.refused()

    def test_refuses_when_already_held(self):
        self.guard.control(None, 'arm')
        self.guard.control(None, 'configure 2160000 0')
        self.guard.commands.clear()
        self.refused()

    def test_refuses_full_turbo_table(self):
        (self.policy / 'scaling_boost_frequencies').write_text('1848000 1896000 2160000 12264000\n')
        self.refused()

    def test_preflight_reports_each_check(self):
        result = hold.preflight(self.root, self.proc, hold.KHZ)
        self.assertTrue(result['ok'])
        self.assertEqual([c['check'] for c in result['checks']],
                         ['cmdline', 'guard', 'profile', 'temperatures', 'turbo-table'])


class LoadTests(Base):
    def load(self, samples):
        self.guard.control(None, 'arm')
        self.guard.control(None, 'configure 2160000 0')
        values = iter(samples)

        def status(root):
            value = next(values, None)
            if value == 'trip':
                self.guard.trip('thermal-passive')
            elif value:
                self.guard.state['actual_khz'] = str(value)
            return dict(self.guard.state)
        clock = iter(range(1000))
        with mock.patch.object(ladder, 'status', status), \
                mock.patch.object(hold.subprocess, 'Popen') as popen, \
                mock.patch.object(hold.os, 'killpg') as killpg, \
                mock.patch.object(hold.shutil, 'which', return_value=None), \
                mock.patch.object(hold.time, 'sleep'), \
                mock.patch.object(hold.time, 'monotonic', side_effect=lambda: next(clock) * 0.25):
            popen.return_value.pid = 4242
            code = hold.load_test(self.root, hold.KHZ, 2)
        self.assertEqual(popen.call_count, 2)
        killpg.assert_called()
        return code, self.events()[-1]

    def test_load_reaching_the_target(self):
        code, result = self.load([None] + [2160000] * 9)
        self.assertEqual(code, 0)
        self.assertGreaterEqual(result['at_target'], 0.8 * result['samples'])

    def test_load_stuck_at_stock_fails(self):
        code, result = self.load([None] + [1800000] * 9)
        self.assertEqual((code, result['ok'], result['max_khz']), (1, False, 1800000))

    def test_revert_during_load_fails(self):
        code, result = self.load([None, 2160000, 'trip'])
        self.assertEqual(code, 1)
        self.assertIn('thermal-passive', result['error'])

    def test_load_without_a_hold_is_refused(self):
        self.guard.trip('user-revert')
        self.assertEqual(hold.load_test(self.root, hold.KHZ, 2), 1)


if __name__ == '__main__':
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    raise SystemExit(not result.wasSuccessful())
