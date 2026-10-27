#!/usr/bin/env python3
"""Host-only three-zone load, handoff and failure cleanup regression suite."""
import errno
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


r3 = module('round3_tests', HERE / 'validate-round3.py')
ladder = r3.ladder
stock = module('stock_log', HERE / 'stock-log.py')


class StockTests(unittest.TestCase):
    def setUp(self):
        r3.previous.LadderTests.setUp(self)
        self.zones = [f'thermal_zone{i}' for i in range(3)]
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        for name, value in {'scaling_driver': 'apple-cpufreq', 'cpuinfo_cur_freq': '1800000',
                            'scaling_cur_freq': '1800000', 'scaling_max_freq': '1800000'}.items():
            (policy / name).write_text(value)

    def check(self, cutoff=80000, zones=None):
        stock.guard(stock.snapshot(self.root), self.zones if zones is None else zones, cutoff)

    def test_stock_three_zones_with_battery_and_quarantined_enodata(self):
        for i, name in enumerate(('battery', *sorted(ladder.ALL_NAMES - ladder.NAMES)), 3):
            zone = self.root / f'class/thermal/thermal_zone{i}'
            zone.mkdir()
            (zone / 'type').write_text(name)
        self.check()

    def test_stock_missing_duplicate_or_substituted_required_sensor(self):
        for zones in (self.zones[:2], [*self.zones[:2], 'thermal_zone1']):
            with self.subTest(zones=zones), self.assertRaises(ValueError):
                self.check(zones=zones)
        path = self.root / 'class/thermal/thermal_zone2/type'
        path.write_text('apple-a9-tcc0')
        with self.assertRaises(ValueError):
            self.check()

    def test_stock_does_not_launch_without_fresh_valid_snapshot(self):
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        original = raw.read_text()
        for before, after in (('trusted_mask=23', 'trusted_mask=3f'),
                              ('trusted_mask=23', 'missing_mask=23'),
                              ('age_ms=0', 'age_ms=501'), ('age_ms=0', 'age_ms=-1'),
                              ('bk=80000780', 'bk=00000780'), ('count=00000100', 'count=0'),
                              ('error=0', 'error=-61'), ('trusted=1', 'trusted=0')):
            with self.subTest(after=after):
                raw.write_text(original.replace(before, after, 1))
                with self.assertRaises(ValueError):
                    self.check()
        raw.unlink()
        with self.assertRaises(ValueError):
            self.check()

    def test_stock_keeps_cooling_and_trip_checks(self):
        path = self.root / 'class/thermal/thermal_zone0/trip_point_0_temp'
        path.write_text('80001')
        with self.assertRaises(ValueError):
            self.check()
        path.write_text('80000')
        (path.parent / 'cdev0').unlink()
        with self.assertRaises(ValueError):
            self.check()

    def test_stock_lowerable_cutoff_cannot_exceed_80c(self):
        for cutoff in (30000, 80001, 95000):
            with self.subTest(cutoff=cutoff), self.assertRaises(ValueError):
                self.check(cutoff)
        self.check(30001)
        (self.root / 'class/thermal/thermal_zone0/temp').write_text('80000')
        with self.assertRaises(ValueError):
            self.check()

    def test_stock_cannot_run_at_unknown_or_experimental_clock(self):
        path = self.root / 'devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq'
        for value in ('0', '1848000', '1992000'):
            with self.subTest(value=value):
                path.write_text(value)
                with self.assertRaises(ValueError):
                    self.check()

    def test_stock_load_stops_on_mid_run_sensor_fault(self):
        initial = stock.snapshot(self.root)
        invalid = dict(initial, thermal_guard_error='stale required sensor')
        child = mock.Mock()
        with mock.patch.object(stock.sys, 'argv', ['stock-log.py', '--stress', '--duration', '1',
                '--soc-zone', self.zones[0], '--soc-zone', self.zones[1], '--soc-zone', self.zones[2]]), \
                mock.patch.object(stock.sys, 'platform', 'linux'), \
                mock.patch.object(stock, 'snapshot', side_effect=[initial, invalid]), \
                mock.patch.object(stock.time, 'monotonic', return_value=0), \
                mock.patch.object(stock.time, 'sleep'), \
                mock.patch.object(stock.subprocess, 'Popen', return_value=child) as launch, \
                mock.patch.object(stock, 'stop_child') as stop:
            self.assertEqual(stock.main(), 1)
            launch.assert_called_once()
            stop.assert_called_with(child)


class HandoffTests(r3.VoltageTests):
    def test_only_observed_handoff_and_new_kernel_are_accepted(self):
        for before, after in (('voltage_ctl=00000000', 'voltage_ctl=00000001'),
                              ('voltage_ctl_boot=00000000', 'voltage_ctl_boot=00000001'),
                              ('voltage_routes_boot=00001782', 'voltage_routes_boot=00000000'),
                              ('voltage_handoff_ok=1', 'voltage_handoff_ok=0'),
                              ('format=3', 'format=2')):
            with self.subTest(after=after):
                path = self.readback_file()
                path.write_text(path.read_text().replace(before, after))
                with self.assertRaises(RuntimeError):
                    ladder.voltage_readback(self.root, 1992000, 6250)

    def test_control_errno_includes_kernel_diagnostics(self):
        self.readback_file()
        with mock.patch.object(Path, 'open', side_effect=OSError(errno.EOPNOTSUPP, 'unsupported')), \
                mock.patch.object(ladder, 'status', return_value={'control_error': '-95',
                    'voltage_handoff_ok': '0'}) as status, mock.patch.object(ladder, 'emit') as emit:
            with self.assertRaises(OSError):
                ladder.control(self.root, 'arm')
            self.assertEqual(emit.call_args.kwargs['errno'], errno.EOPNOTSUPP)
            self.assertEqual(emit.call_args.kwargs['guard']['control_error'], '-95')
            status.assert_called_once()

    def test_guard_check_cleans_up_after_mid_test_sensor_failure(self):
        with mock.patch.object(ladder, 'sensors', side_effect=[({}, ''), RuntimeError('sensor lost')]), \
                mock.patch.object(ladder, 'voltage_handoff'), \
                mock.patch.object(ladder, 'status', return_value={}), \
                mock.patch.object(ladder, 'ensure_stock', return_value={}), \
                mock.patch.object(ladder, 'control') as control:
            with self.assertRaisesRegex(RuntimeError, 'sensor lost'):
                ladder.guard_check(self.root)
            self.assertEqual([call.args[1] for call in control.call_args_list],
                             ['arm', 'revert', 'arm', 'revert'])

    def test_guard_check_manual_and_timeout_success(self):
        timed_out = {'latched': '1', 'reason': 'heartbeat-timeout'}
        with mock.patch.object(ladder, 'sensors', return_value=({}, '')), \
                mock.patch.object(ladder, 'voltage_handoff'), \
                mock.patch.object(ladder, 'status', return_value={}), \
                mock.patch.object(ladder, 'ensure_stock', side_effect=[{}, {}, timed_out, {}]), \
                mock.patch.object(ladder.time, 'monotonic', side_effect=[0, 0, 3]), \
                mock.patch.object(ladder.time, 'sleep'), \
                mock.patch.object(ladder, 'control') as control, mock.patch.object(ladder, 'emit') as emit:
            ladder.guard_check(self.root)
            self.assertEqual([c.args[0] for c in emit.call_args_list],
                             ['manual-revert-pass', 'heartbeat-revert-pass', 'stock-restored'])
            self.assertEqual([c.args[1] for c in control.call_args_list], ['arm', 'revert', 'arm', 'revert'])

    def test_watchdog_reset_check_never_arms_and_never_reports_a_pass(self):
        watchdog = mock.Mock(timeout=10)
        with mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder, 'sensors'), \
                mock.patch.object(ladder, 'boot_id', return_value='test-boot'), \
                mock.patch.object(ladder, 'Watchdog', return_value=watchdog), \
                mock.patch.object(ladder.time, 'sleep'), mock.patch.object(ladder, 'control') as control:
            with self.assertRaisesRegex(RuntimeError, 'did not reset'):
                ladder.watchdog_check(self.root, Path('/dev/watchdog0'))
            watchdog.abandon.assert_called_once()
            watchdog.close.assert_not_called()
            control.assert_not_called()


if __name__ == '__main__':
    r3.previous.kernel_helpers()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
                               for cls in (r3.previous.LadderTests, StockTests, HandoffTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
