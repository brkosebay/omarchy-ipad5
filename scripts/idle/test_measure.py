#!/usr/bin/env python3
"""Host-only tests for recorder math, unavailable gauges and brightness rollback."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('measure', Path(__file__).with_name('measure.py'))
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)
BATTERY = Path('/sys/class/power_supply/bq27540-0')
STATE = '/sys/devices/system/cpu/cpu0/cpuidle/state0/'


def row(t, current=-100000, voltage=4000000, status='Discharging', idle=0):
    return {'monotonic_s': t, 'values': {
        str(BATTERY / 'current_now'): current,
        str(BATTERY / 'voltage_now'): voltage,
        str(BATTERY / 'status'): status,
        STATE + 'name': 'WFI', STATE + 'time': idle, STATE + 'usage': int(t),
    }, 'cpu_ticks': {'cpu0': [100, 0, 100, int(t * 100), 0, 0, 0, 0, 1000, 0]}}


class RecorderTests(unittest.TestCase):
    def test_units_and_cpu_residency(self):
        result = measure.summarize([row(0), row(3600, idle=1800000000)], BATTERY)
        self.assertAlmostEqual(result['discharge_energy_mWh'], 400)
        self.assertEqual(result['current_now_uA_signed']['mean'], -100000)
        self.assertEqual(result['cpuidle'][STATE]['wall_percent'], 50)
        self.assertEqual(result['proc_stat']['cpu0']['idle_percent'], 100)
        self.assertTrue(result['battery_comparison_eligible'])

    def test_trapezoid_uses_time_not_sample_mean(self):
        result = measure.summarize([row(0, current=0), row(1, current=-100000),
                                    row(10, current=-100000)], BATTERY)
        self.assertEqual(result['current_now_uA_signed']['mean'], -95000)

    def test_charging_mixed_or_missing_data_excluded(self):
        for changed in (row(10, status='Charging'), row(10, current=None),
                        row(10, voltage=None)):
            result = measure.summarize([row(0), changed], BATTERY)
            self.assertFalse(result['battery_comparison_eligible'])
            self.assertIsNone(result['discharge_power_uW_magnitude']['mean'])

    def test_online_supply_excludes_comparison(self):
        rows = [row(0), row(10)]
        rows[0]['values']['/sys/class/power_supply/usb/online'] = 1
        self.assertFalse(measure.summarize(rows, BATTERY)['battery_comparison_eligible'])

    def test_counter_reset_and_missing_state(self):
        result = measure.summarize([row(0, idle=100), row(10)], BATTERY)
        self.assertTrue(result['cpuidle'][STATE]['counter_reset'])
        self.assertIsNone(result['cpuidle'][STATE]['time_us'])
        rows = [row(0), row(10)]
        rows[0]['values'].pop(STATE + 'time')
        self.assertEqual(measure.summarize(rows, BATTERY)['cpuidle'], {})

    def test_one_sample_does_not_claim_energy_comparison(self):
        self.assertFalse(measure.summarize([row(0)], BATTERY)['battery_comparison_eligible'])

    def test_brightness_restored_on_interruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            p = root / 'class/backlight/panel/brightness'
            p.parent.mkdir(parents=True)
            p.write_text('77\n')
            with patch.object(measure, 'SYS', root):
                with self.assertRaises(KeyboardInterrupt):
                    with measure.backlight(True):
                        self.assertEqual(p.read_text().strip(), '0')
                        raise KeyboardInterrupt
            self.assertEqual(p.read_text().strip(), '77')

    def test_partial_brightness_setup_failure_restores_previous_device(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / 'class/backlight/a/brightness'
            second = root / 'class/backlight/b/brightness'
            first.parent.mkdir(parents=True)
            second.mkdir(parents=True)  # Cannot read this as a brightness file.
            first.write_text('55\n')
            with patch.object(measure, 'SYS', root):
                with self.assertRaises(RuntimeError):
                    with measure.backlight(True):
                        self.fail('partial setup should fail before yielding')
            self.assertEqual(first.read_text().strip(), '55')

    def test_read_error_is_missing_not_zero(self):
        self.assertIsNone(measure.read(Path('/this/path/does/not/exist')))

    def test_recorder_end_to_end_with_fake_sysfs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sys = root / 'sys'
            battery = sys / 'class/power_supply/gauge'
            battery.mkdir(parents=True)
            for name, value in {'type': 'Battery', 'status': 'Discharging',
                                'current_now': '-100000', 'voltage_now': '4000000'}.items():
                (battery / name).write_text(value)
            proc = root / 'proc'
            proc.mkdir()
            (proc / 'stat').write_text('cpu0 1 0 1 50 0 0 0 0 0 0\n')
            (sys / 'devices').mkdir()
            output = root / 'recording'
            argv = ['measure.py', str(output), '--mode', 'desktop',
                    '--duration', '0.02', '--interval', '0.01', '--settle', '0']
            with patch.object(measure, 'SYS', sys), patch.object(measure, 'PROC', proc), \
                    patch('sys.argv', argv), patch('sys.stdout', io.StringIO()):
                self.assertEqual(measure.main(), 0)
            result = measure.json.loads((output / 'summary.json').read_text())
            self.assertTrue(result['battery_comparison_eligible'])
            self.assertGreaterEqual(result['samples'], 2)
            self.assertEqual(result['current_now_uA_signed']['mean'], -100000)
            for name in ('metadata.json', 'samples.jsonl', 'before.json', 'after.json'):
                self.assertTrue((output / name).is_file())


if __name__ == '__main__':
    unittest.main()
