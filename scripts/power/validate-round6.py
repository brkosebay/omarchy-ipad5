#!/usr/bin/env python3
"""Host-only round-6 tests, retaining the real guard/MMIO and thermal coverage."""
import contextlib
import io
import types
import unittest
from unittest import mock

import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('r5_tests', HERE / 'validate-round5.py')
r5 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r5)
ladder = r5.ladder


class Round6Tests(r5.DeliveryTests):
    def test_single_core_workload_keeps_checks_and_cleanup(self):
        self.exercise([1848000, 1848000], [0, 0, 1], cores=1)

    def test_seconds_and_cores_reach_trial_including_stock_control(self):
        args = types.SimpleNamespace(sysfs=self.root, logdir=self.root, seconds=60, minutes=5,
            cores=1, max_delta_uv=50000, voltage_step_uv=6250, control_seconds=30)
        with mock.patch.object(ladder, 'trial', side_effect=[ladder.ClockFailure('verify'), None, None]) as trial, \
                mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder, 'cooldown'):
            self.assertEqual(ladder.trial_with_offsets(args, None, 2400000, None), 6250)
            self.assertEqual([c.args[3] for c in trial.call_args_list], [60, 30, 60])
            self.assertEqual([c.kwargs for c in trial.call_args_list], [{'cores': 1}] * 3)
            self.assertEqual(trial.call_args_list[-1].args[-1], 6250)

    def test_cli_duration_compatibility_and_range(self):
        args = ladder.parse_args(['ladder', '--cores', '1', '--seconds', '60',
            '--from-khz', '2208000', '--through-khz', '12264000'])
        self.assertEqual((args.cores, ladder.trial_seconds(args)), (1, 60))
        self.assertEqual((args.from_khz, args.through_khz), (2208000, 12264000))
        self.assertEqual(ladder.trial_seconds(ladder.parse_args(['ladder', '--minutes', '2'])), 120)
        self.assertEqual(ladder.trial_seconds(ladder.parse_args(['ladder'])), 300)
        for flags in (['--seconds', '60', '--minutes', '2'], ['--cores', '0'],
                      ['--through-khz', '12288000'], ['--through-khz', '2184000']):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                ladder.parse_args(['ladder', *flags])

    def test_confirmation_selects_exactly_one_extended_clock(self):
        policy = self.root / 'devices/system/cpu/cpufreq/policy0'
        policy.mkdir(parents=True)
        for name, value in {'scaling_driver': 'apple-cpufreq', 'scaling_governor': 'schedutil',
                            'scaling_min_freq': '300000', 'scaling_max_freq': '1800000'}.items():
            (policy / name).write_text(value)
        args = ladder.parse_args(['ladder', '--sysfs', str(self.root), '--logdir', str(self.root / 'logs'),
            '--from-khz', '2400000', '--through-khz', '2400000', '--cores', '2', '--seconds', '120'])
        watchdog = mock.Mock()
        with mock.patch.object(ladder.shutil, 'which', return_value='/fake/stress-ng'), \
                mock.patch.object(ladder, 'status', return_value={'enable': '1'}), \
                mock.patch.object(ladder, 'voltage_handoff'), \
                mock.patch.object(ladder, 'Watchdog', return_value=watchdog), \
                mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder, 'control'), \
                mock.patch.object(ladder, 'cooldown'), mock.patch.object(ladder, 'trial') as trial:
            ladder.run_trials(args)
        trial.assert_called_once_with(self.root, policy, 2400000, 120, args.logdir, watchdog, 0, cores=2)
        watchdog.close.assert_called_once()
        watchdog.abandon.assert_not_called()

    def test_shared_slot_readback_all_rungs_and_offsets(self):
        self.assertEqual(ladder.STEPS[-1], 511 * 24000)
        self.assertIn(2400000, ladder.STEPS)
        for target in ladder.STEPS[7:]:
            for delta in (0, 6250, 50000):
                path = r5.r4.r3.VoltageTests.readback_file(self, delta)
                old1 = (0x74000000002024b0 & ~(511 << 4 | 255 << 56)) | 83 << 4 | (116 + delta // 3125) << 56
                new1 = (old1 & ~(511 << 4)) | (target // 24000) << 4
                path.write_text(path.read_text().replace('state=11', 'state=14').replace(f'{old1:016x}', f'{new1:016x}'))
                self.assertEqual(ladder.voltage_readback(self.root, target, delta)['state'], '14')
                path.write_text(path.read_text().replace(f'{new1:016x}', f'{old1:016x}'))
                with self.assertRaisesRegex(RuntimeError, 'readback mismatch'):
                    ladder.voltage_readback(self.root, target, delta)


if __name__ == '__main__':
    r5.r4.r3.previous.kernel_helpers()
    r5.trip_helpers()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(cls)
        for cls in (r5.r4.r3.previous.LadderTests, r5.r4.HandoffTests, r5.ProfileTests, r5.DeliveryTests))
    suite.addTests(Round6Tests(name) for name in Round6Tests.__dict__ if name.startswith('test_'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
