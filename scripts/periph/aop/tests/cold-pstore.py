#!/usr/bin/env python3
"""Run the existing real pstore reserve/boot-order/no-replay fixtures; offline."""
from pathlib import Path
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parents[4]
source = sys.argv[1] if len(sys.argv) == 2 else '/src/wt/tri49a'
script = ROOT / 'scripts/boot/test-recovery-offline.py'
sys.argv = [str(script), '--source', source]
tests = runpy.run_path(str(script))['RecoveryTests']
suite = unittest.TestSuite(tests(name) for name in (
    'test_early_pstore_with_real_libfdt', 'test_pstore_boot_phase_order',
    'test_pstore_no_replay_copy_idempotence_and_archive'))
if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
    raise SystemExit(1)
