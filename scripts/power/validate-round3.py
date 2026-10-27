#!/usr/bin/env python3
"""Offline voltage/readback/retry fault tests plus the round-2 guard suite."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('round2_tests', HERE / 'validate-round2.py')
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
ladder = previous.ladder


class VoltageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.args = types.SimpleNamespace(sysfs=self.root, logdir=self.root, minutes=1,
            max_delta_uv=50000, voltage_step_uv=6250, control_seconds=30)
        capture = contextlib.redirect_stdout(io.StringIO())
        capture.__enter__()
        self.addCleanup(capture.__exit__, None, None, None)

    def test_stock_pass_never_adds_voltage(self):
        with mock.patch.object(ladder, 'trial') as trial:
            self.assertEqual(ladder.trial_with_offsets(self.args, None, 1992000, None), 0)
            self.assertEqual([c.args[-1] for c in trial.call_args_list], [0])

    def test_work_failure_requires_stock_control_then_small_offset(self):
        with mock.patch.object(ladder, 'trial', side_effect=[ladder.ClockFailure('verify'), None, None]) as trial, \
                mock.patch.object(ladder, 'ensure_stock') as stock, \
                mock.patch.object(ladder, 'cooldown'):
            self.assertEqual(ladder.trial_with_offsets(self.args, None, 1992000, None), 6250)
            self.assertEqual([c.args[2] for c in trial.call_args_list], [1992000, 1800000, 1992000])
            self.assertEqual(trial.call_args_list[-1].args[-1], 6250)
            stock.assert_called_once()

    def test_safety_errors_never_increase_voltage(self):
        for error in ('thermal', 'sensor', 'heartbeat', 'readback', 'undelivered', 'revert', 'watchdog'):
            with self.subTest(error=error), mock.patch.object(ladder, 'trial', side_effect=RuntimeError(error)) as trial:
                with self.assertRaisesRegex(RuntimeError, error):
                    ladder.trial_with_offsets(self.args, None, 1992000, None)
                self.assertEqual(trial.call_count, 1)

    def test_failed_stock_control_stops_retry(self):
        with mock.patch.object(ladder, 'trial', side_effect=[ladder.ClockFailure('verify'), ladder.ClockFailure('stock')]) as trial, \
                mock.patch.object(ladder, 'ensure_stock'), mock.patch.object(ladder, 'cooldown'):
            with self.assertRaisesRegex(ladder.ClockFailure, 'stock'):
                ladder.trial_with_offsets(self.args, None, 1992000, None)
            self.assertEqual(trial.call_count, 2)

    def test_ceiling_and_boost_failures_stop(self):
        for target, delta in ((1848000, 0), (2160000, 50000)):
            with self.subTest(target=target), mock.patch.object(ladder, 'trial', side_effect=ladder.ClockFailure('verify')) as trial, \
                    mock.patch.object(ladder, 'ensure_stock'):
                with self.assertRaises(ladder.ClockFailure):
                    ladder.trial_with_offsets(self.args, None, target, None, delta)
                self.assertEqual(trial.call_count, 1)

    def test_unconfirmed_revert_blocks_retry(self):
        with mock.patch.object(ladder, 'trial', side_effect=ladder.ClockFailure('verify')) as trial, \
                mock.patch.object(ladder, 'ensure_stock', side_effect=RuntimeError('unconfirmed')):
            with self.assertRaisesRegex(RuntimeError, 'unconfirmed'):
                ladder.trial_with_offsets(self.args, None, 1992000, None)
            self.assertEqual(trial.call_count, 1)

    def test_unknown_clock_cannot_confirm_revert(self):
        state = {'armed': '0', 'dirty': '0', 'configured': '0', 'delta_uv': '0',
                 'revert_error': '0', 'actual_khz': '0'}
        with mock.patch.object(ladder, 'status', return_value=state), self.assertRaises(RuntimeError):
            ladder.ensure_stock(self.root)

    def readback_file(self, delta=6250):
        path = self.root / 'kernel/debug/a9_tuning/status'
        path.parent.mkdir(parents=True, exist_ok=True)
        base1, base2 = 0x74000000002024b0, 0x67000f650d000000
        word1 = (base1 & ~(511 << 4 | 255 << 56)) | 83 << 4 | (116 + delta // 3125) << 56
        word2 = (base2 & ~(255 << 32)) | (101 + delta // 3125) << 32
        path.write_text(f'format=3 clock_profile={ladder.CLOCK_PROFILE} boost_state=15 dac_step_uv=3125 ceiling_delta_uv=50000 delta_uv={delta}\n'
            'cmd=0 voltage_ctl=00000000 voltage_routes=00001782 voltage_ctl_boot=00000000 voltage_routes_boot=00001782 voltage_handoff_ok=1 handoff_policy=s8003-r4\n'
            f'state=7 boot_psinfo1={base1:016x} boot_psinfo2={base2:016x}\n'
            f'state=11 boot_psinfo1=0 boot_psinfo2=0 psinfo1={word1:016x} psinfo2={word2:016x} '
            f'core_code={116+delta//3125} aux_code={101+delta//3125} '
            f'core_nominal_uv={812500+delta} aux_nominal_uv={915625+delta}\n')
        return path

    def test_readback_both_rails_and_full_words(self):
        self.readback_file()
        row = ladder.voltage_readback(self.root, 1992000, 6250)
        self.assertEqual(row['aux_nominal_uv'], '921875')

    def test_readback_wrong_code_or_timing_stops(self):
        for before, after in (('7600000000202530', '7700000000202530'),
                              ('67000f670d000000', '67000f680d000000'),
                              ('7600000000202530', '7600000000302530'),
                              ('aux_code=103', 'aux_code=104'),
                              ('voltage_routes=00001782', 'voltage_routes=00001783'),
                              ('ceiling_delta_uv=50000', 'ceiling_delta_uv=60000')):
            with self.subTest(before=before):
                path = self.readback_file()
                self.assertIn(before, path.read_text())
                path.write_text(path.read_text().replace(before, after))
                with self.assertRaises(RuntimeError):
                    ladder.voltage_readback(self.root, 1992000, 6250)

    def test_resume_needs_new_boot_and_unfinished_trial(self):
        path = self.root / 'interrupted.jsonl'
        start = {'event': 'trial-start', 'target_khz': 1992000, 'delta_uv': 0, 'boot_id': 'old'}
        path.write_text(json.dumps(start) + '\n')
        with mock.patch.object(ladder, 'boot_id', return_value='new'):
            self.assertEqual(ladder.retry_candidate(path), (1992000, 0))
        with mock.patch.object(ladder, 'boot_id', return_value='old'):
            with self.assertRaises(ValueError):
                ladder.retry_candidate(path)
        for event in ('failure', 'summary', 'trial-pass', 'stock-restored', 'stock-unconfirmed'):
            path.write_text(json.dumps(start) + '\n' + json.dumps({'event': event}) + '\n')
            with mock.patch.object(ladder, 'boot_id', return_value='new'), self.assertRaises(ValueError):
                ladder.retry_candidate(path)


if __name__ == '__main__':
    previous.kernel_helpers()
    suite = unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromTestCase(c)
                              for c in (previous.LadderTests, VoltageTests)])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
