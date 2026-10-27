#!/usr/bin/env python3
"""Host-only profile, thermal-core trip setter and load-tool regression tests."""
import subprocess
import unittest
from unittest import mock

import a9_thermal
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
# Reuse the three-zone, handoff, watchdog and voltage-failure coverage.
import importlib.util
spec = importlib.util.spec_from_file_location('round4_tests', HERE / 'validate-round4.py')
r4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r4)
ladder, stock = r4.ladder, r4.stock


def trip_helpers():
    """Compile the actual callback/application code against a locked trip model."""
    source = (HERE / 'kernel/apple-s8003-thermal.c').read_text()
    shared = (HERE / 'kernel/s8003-power.h').read_text()
    body = r4.r3.previous.body
    code = '''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#define BIT(n) (1U << (n))
#define THERMAL_EVENT_UNSPECIFIED 0
enum { THERMAL_TRIP_PASSIVE, THERMAL_TRIP_HOT, THERMAL_TRIP_CRITICAL, THERMAL_TRIP_ACTIVE };
struct thermal_trip { int type, temperature; };
struct thermal_zone_device { struct thermal_trip trips[4]; int count; bool locked; };
static int stores, updates;
static int thermal_zone_for_each_trip(struct thermal_zone_device *tz,
        int (*cb)(struct thermal_trip *, void *), void *data) {
    tz->locked=true;
    int ret=0;
    for (int i=0;i<tz->count;i++) if ((ret=cb(&tz->trips[i],data))) break;
    tz->locked=false;
    return ret;
}
static void thermal_zone_set_trip_temp(struct thermal_zone_device *tz,
        struct thermal_trip *trip, int temp) {
    assert(tz->locked && trip->type!=THERMAL_TRIP_CRITICAL);
    trip->temperature=temp;stores++;
}
static void thermal_zone_device_update(struct thermal_zone_device *tz, int event) {
    assert(!tz->locked);updates++;
}
'''
    code += body(shared, 'struct apple_a9_thermal_profile {') + ';\n'
    code += body(shared, 'static inline struct apple_a9_thermal_profile apple_a9_stock_thermal_profile')
    code += '\nstatic struct apple_a9_thermal_profile selected;\n'
    code += 'static struct apple_a9_thermal_profile apple_a9_get_thermal_profile(void) { return selected; }\n'
    code += body(source, 'struct a9_trip_profile {') + ';\n'
    for marker in ('static int a9_profile_trip', 'static int a9_apply_profile'):
        code += body(source, marker) + '\n'
    code += '''
int main(void) {
    struct thermal_zone_device stock={.trips={{THERMAL_TRIP_PASSIVE,80000},
        {THERMAL_TRIP_HOT,90000},{THERMAL_TRIP_CRITICAL,100000}},.count=3};
    selected=apple_a9_stock_thermal_profile();
    struct thermal_zone_device tz=stock;
    assert(a9_apply_profile(&tz)==0 && !stores && !updates);
    selected.tuning=true;selected.hot=95000;
    for (int passive=70000;passive<=92000;passive+=1000) {
        selected.passive=passive;
        for (int zone=0;zone<3;zone++) {
            tz=stock;stores=updates=0;
            assert(a9_apply_profile(&tz)==0 && stores==2 && updates==1);
            assert(tz.trips[0].temperature==passive);
            assert(tz.trips[1].temperature==95000 && tz.trips[2].temperature==100000);
        }
    }
    for (int i=0;i<3;i++) {
        tz=stock;tz.trips[i].temperature++;stores=updates=0;
        assert(a9_apply_profile(&tz)==-EINVAL && !stores && !updates);
    }
    tz=stock;tz.count=2;assert(a9_apply_profile(&tz)==-EINVAL && !stores);
    tz=stock;tz.count=4;tz.trips[3]=tz.trips[0];
    assert(a9_apply_profile(&tz)==-EINVAL && !stores);
    tz=stock;tz.trips[0].type=THERMAL_TRIP_ACTIVE;
    assert(a9_apply_profile(&tz)==-EINVAL && !stores);
}
'''
    out = ROOT / 'build/power/oc-r5/tests'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'thermal-trips.c').write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-O1', '-g0', '-fsanitize=address,undefined',
                    str(out / 'thermal-trips.c'), '-o', str(out / 'thermal-trips')], check=True)
    subprocess.run([str(out / 'thermal-trips')], check=True)


class ProfileTests(r4.StockTests):
    def select(self, passive=90000, tuning=True):
        name = ('sustained' if passive == 90000 else 'custom') if tuning else 'stock'
        header = (f'thermal_profile_format=1 profile={name} profile_tuning={int(tuning)} '
                  f'passive_millic={passive} hot_millic={95000 if tuning else 90000} '
                  f'critical_millic=100000 arm_millic={passive-5000} cooldown_millic={passive-10000}')
        path = self.root / 'kernel/debug/a9_thermal/snapshot'
        rows = path.read_text().splitlines()[1:]
        path.write_text(f'format=3 trusted_mask=23 profile_ready=1 {header}\n' + '\n'.join(rows))
        self.guard_path = self.root / 'kernel/debug/a9_tuning/status'
        self.guard_path.parent.mkdir(exist_ok=True)
        self.guard_path.write_text(f'format=3 enable={int(tuning)} {header}\ncmd=0\n')
        for zone in (self.root / 'class/thermal').glob('thermal_zone*'):
            (zone / 'trip_point_0_temp').write_text(str(passive))
            (zone / 'trip_point_1_temp').write_text('95000' if tuning else '90000')

    def temp(self, temp):
        for zone in (self.root / 'class/thermal').glob('thermal_zone*'):
            (zone / 'temp').write_text(str(temp))

    def test_sustained_accepts_above_80_and_aborts_at_90(self):
        self.select()
        self.temp(89999)
        sample = stock.snapshot(self.root)
        self.assertEqual(sample['thermal_profile']['profile'], 'sustained')
        stock.guard(sample, self.zones)
        stock.guard(sample, self.zones, 90000)
        for cutoff in (80000, 90001, 95000):
            with self.subTest(cutoff=cutoff), self.assertRaises(ValueError):
                stock.guard(sample, self.zones, cutoff)
        self.temp(90000)
        with self.assertRaises(RuntimeError):
            ladder.sensors(self.root)

    def test_all_custom_profiles_and_signed_temperatures(self):
        for passive in (70000, 80000, 90000, 92000):
            with self.subTest(passive=passive):
                self.select(passive)
                self.temp(passive - 1)
                stock.guard(stock.snapshot(self.root), self.zones)
                self.temp(-1000)
                ladder.sensors(self.root)
                self.temp(passive)
                with self.assertRaises(RuntimeError):
                    ladder.sensors(self.root)

    def test_unready_old_or_inconsistent_profiles_fail_closed(self):
        self.select()
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        original = raw.read_text()
        for before, after in (('format=3', 'format=2'), ('profile_ready=1', 'profile_ready=0'),
                ('passive_millic=90000', 'passive_millic=92001'),
                ('passive_millic=90000', 'passive_millic=69999'),
                ('hot_millic=95000', 'hot_millic=95001'),
                ('critical_millic=100000', 'critical_millic=100001'),
                ('arm_millic=85000', 'arm_millic=85001'),
                ('cooldown_millic=80000', 'cooldown_millic=80001'),
                ('profile=sustained', 'profile=stock'),
                ('profile_tuning=1', 'profile_tuning=2'),
                ('passive_millic=90000', 'missing=90000')):
            with self.subTest(after=after):
                raw.write_text(original.replace(before, after, 1))
                with self.assertRaises(RuntimeError):
                    ladder.sensors(self.root)
        raw.write_text(original)

    def test_guard_or_zone_mismatch_blocks_tuning(self):
        self.select()
        original = self.guard_path.read_text()
        for before, after in (('passive_millic=90000', 'passive_millic=80000'),
                              ('enable=1', 'enable=0'),
                              ('thermal_profile_format=1', 'thermal_profile_format=0')):
            self.guard_path.write_text(original.replace(before, after))
            with self.assertRaises(RuntimeError):
                ladder.sensors(self.root)
        self.guard_path.write_text(original)
        self.guard_path.unlink()
        with self.assertRaises(RuntimeError):
            ladder.sensors(self.root)
        self.select()
        for point, bad in ((0, 80000), (1, 90000), (2, 99999), (2, 100001)):
            path = self.root / f'class/thermal/thermal_zone0/trip_point_{point}_temp'
            original = path.read_text()
            path.write_text(str(bad))
            with self.assertRaises(RuntimeError):
                ladder.sensors(self.root)
            path.write_text(original)

    def test_raw_temperature_cannot_bypass_profile_cutoff(self):
        self.select()
        raw = self.root / 'kernel/debug/a9_thermal/snapshot'
        raw.write_text(raw.read_text().replace('temp=30000', 'temp=90000', 1))
        with self.assertRaisesRegex(RuntimeError, 'invalid/stale'):
            ladder.sensors(self.root)

    def test_stock_boot_ignores_no_guard_but_not_mismatched_guard(self):
        self.select(80000, False)
        ladder.sensors(self.root)
        self.guard_path.unlink()
        ladder.sensors(self.root)

    def test_trial_arm_threshold_tracks_profile_before_any_write(self):
        self.select()
        policy = self.root / 'policy'
        policy.mkdir()
        self.temp(85000)
        with mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder, 'control') as control:
            with self.assertRaisesRegex(RuntimeError, '85000 milliC'):
                ladder.trial(self.root, policy, 1848000, 1, self.root, mock.Mock())
            control.assert_not_called()

    def test_cooldown_uses_80_for_sustained_and_70_for_stock(self):
        for passive, tuning, expected in ((90000, True, 80000), (80000, False, 70000)):
            self.select(passive, tuning)
            watchdog = mock.Mock()
            self.temp(expected)
            def cool(seconds):
                self.temp(expected - 1)
            with mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder.time, 'sleep', side_effect=cool):
                ladder.cooldown(self.root, watchdog)
            watchdog.ping.assert_called_once()

    def test_stock_main_defaults_to_selected_passive_cutoff(self):
        self.select()
        self.temp(85000)
        sample = stock.snapshot(self.root)
        child = mock.Mock()
        with mock.patch.object(stock.sys, 'argv', ['stock-log.py', '--stress', '--duration', '1',
                *sum((['--soc-zone', z] for z in self.zones), [])]), \
                mock.patch.object(stock.sys, 'platform', 'linux'), \
                mock.patch.object(stock, 'snapshot', return_value=sample), \
                mock.patch.object(stock.time, 'monotonic', side_effect=[0, 1]), \
                mock.patch.object(stock.subprocess, 'Popen', return_value=child) as launch, \
                mock.patch.object(stock, 'stop_child'), mock.patch.object(stock, 'emit') as emit:
            self.assertEqual(stock.main(), 0)
            launch.assert_called_once()
            self.assertEqual(emit.call_args_list[0].args[0]['cutoff_millic'], 90000)


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        r4.r3.VoltageTests.setUp(self)

    def test_1848_readback_uses_all_load_slot_15_and_refuses_old_mapping(self):
        path = r4.r3.VoltageTests.readback_file(self, 0)
        source = path.read_text().replace('state=11', 'state=15').replace(
            'psinfo1=7400000000202530', 'psinfo1=74000000002024d0')
        path.write_text(source)
        self.assertEqual(ladder.voltage_readback(self.root, 1848000, 0)['state'], '15')
        for before, after in (('boost_state=15', 'boost_state=8'),
                              (f'clock_profile={ladder.CLOCK_PROFILE}', 'old_profile=1')):
            path.write_text(source.replace(before, after))
            with self.assertRaisesRegex(RuntimeError, 'round-6'):
                ladder.voltage_readback(self.root, 1848000, 0)

    def exercise(self, actual, times, error=None, cores=2):
        policy = self.root / 'policy'
        policy.mkdir()
        (self.root / 'devices/system/cpu/cpufreq').mkdir(parents=True)
        child = mock.Mock(returncode=0)
        child.poll.side_effect = [None, 0]
        watchdog = mock.Mock(timeout=10)
        samples = [{'guard': {'armed': '1', 'latched': '0'}, 'actual_khz': freq} for freq in actual]
        with mock.patch.object(ladder, 'ensure_stock'), \
                mock.patch.object(ladder.a9_thermal, 'profile', return_value={'arm_millic': 85000}), \
                mock.patch.object(ladder, 'sensors', return_value=({'test': 30000}, '')), \
                mock.patch.object(ladder, 'voltage_readback', return_value={}), \
                mock.patch.object(ladder, 'boot_id', return_value='test-boot'), \
                mock.patch.object(ladder, 'control') as control, \
                mock.patch.object(ladder, 'telemetry', side_effect=samples), \
                mock.patch.object(ladder.subprocess, 'Popen', return_value=child) as launch, \
                mock.patch.object(ladder, 'stop_child') as stop, \
                mock.patch.object(ladder.time, 'sleep'), \
                mock.patch.object(ladder.time, 'monotonic', side_effect=times), \
                mock.patch.object(ladder, 'emit'):
            if error:
                with self.assertRaisesRegex(RuntimeError, error):
                    ladder.trial(self.root, policy, 1848000, 1, self.root, watchdog, cores=cores)
            else:
                ladder.trial(self.root, policy, 1848000, 1, self.root, watchdog, cores=cores)
            self.assertIn('configure 1848000 0', [c.args[1] for c in control.call_args_list])
            self.assertEqual(control.call_args_list[-1].args[1], 'revert')
            self.assertEqual(launch.call_args.args[0][1:3], ['--cpu', str(cores)])
            stop.assert_called_once_with(child)
            return watchdog

    def test_1848_trial_configures_before_governor_and_runs_two_workers(self):
        self.exercise([1848000, 1848000], [0, 0, 1])

    def test_undelivered_target_aborts_after_ten_seconds_and_reverts(self):
        watchdog = self.exercise([1800000], [0, 10], 'never delivered within 10 s')
        watchdog.ping.assert_not_called()

    def test_lost_delivery_also_aborts_instead_of_running_at_stock(self):
        self.exercise([1848000, 1800000], [0, 0, 10], 'stopped being delivered within 10 s')


if __name__ == '__main__':
    r4.r3.previous.kernel_helpers()
    trip_helpers()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
        for cls in (r4.r3.previous.LadderTests, r4.HandoffTests, ProfileTests, DeliveryTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
