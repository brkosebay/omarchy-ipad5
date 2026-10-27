#!/usr/bin/env python3
"""Re-run the production sleep stop/rollback fixture with ans_keep_power=1.

Host/Docker only; --boot selects the prepared kernel tree. The imported
fixture extracts the actual shared PCIe helper, not a rewritten model.
"""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).parents[2] / 'power/test-s2idle-round1.py'
spec = importlib.util.spec_from_file_location('aop_sleep', path)
sleep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sleep)


class KeepPowerSleep(unittest.TestCase):
    def run_c(self, source, *args, **kwargs):
        anchor = 'init();inactive=1;host.hw=&hw;host.sleep_ports=3;'
        assert anchor in source
        source = source.replace(anchor, 'init();ans_keep_power=true;inactive=1;host.hw=&hw;host.sleep_ports=3;')
        # Subsequent fault cases also retain the option (init does not reset it).
        sleep.SleepTests.run_c(self, source, *args, **kwargs)

    def test_powered_kexec_option_never_changes_suspend_certificate(self):
        sleep.SleepTests.test_port_certificate_guard_stop_and_rollback(self)


if __name__ == '__main__':
    unittest.main(argv=['test-nextint-sleep', *sleep.rest], verbosity=2)
