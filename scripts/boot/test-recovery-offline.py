#!/usr/bin/env python3
"""Production C/libfdt and shell fixtures for Round 5; no device access."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path('/src/wt/boot'))
args = parser.parse_args()
KERNEL = args.source


def function(path, name):
    src = (KERNEL/path).read_text()
    start = re.search(r'static (?:void|int|ssize_t) (?:__init )?'+name+r'\(', src).start()
    return src[start:src.index('\n}', start)+2]


class RecoveryTests(unittest.TestCase):
    def test_early_pstore_with_real_libfdt(self):
        src = function('arch/arm64/mm/init.c', 'apple_j71t_reserve_pstore')
        text = (KERNEL/'arch/arm64/mm/init.c').read_text()
        src = 'static phys_addr_t apple_j71t_pstore_fdt;\n'+src
        if 'void __init apple_j71t_publish_pstore(' in text:
            start = text.index('void __init apple_j71t_publish_pstore(')
            src += '\n'+text[start:text.index('\n}', start)+2]
        else:
            # Also run against the faulty Round 5 source: the guarded allocator
            # must reject its pre-paging linear-map access, not hide it again.
            src += '\nstatic void apple_j71t_publish_pstore(void) {}'
        text = (KERNEL/'drivers/of/of_reserved_mem.c').read_text()
        start = text.index('void __init fdt_scan_reserved_mem_late(')
        src += '\n'+text[start:text.index('\n}', start)+2]
        lib = KERNEL/'scripts/dtc/libfdt'
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p/'production.h').write_text(src)
            files = [lib/f'fdt{suffix}.c' for suffix in ('', '_ro', '_rw', '_sw', '_wip', '_empty_tree', '_addresses', '_strerror')]
            subprocess.run(['clang', '-Werror=implicit-function-declaration', '-I', str(lib), '-I', d,
                            str(HERE/'tests/pstore-reserve.c'), *map(str, files), '-o', str(p/'test')], check=True)
            subprocess.run([str(p/'test')], check=True)

    def test_pstore_boot_phase_order(self):
        subprocess.run(['python3', '-B', '-I', str(HERE/'check-early-pstore.py'),
                        '--source', str(KERNEL)], check=True)

    def test_phoenix_production_controller_and_guards(self):
        src = '\n'.join(function('drivers/pci/controller/pcie-apple-h9p.c', n) for n in
                        ('apple_h9p_crash_stop_ans', 'crash_quiesce_store', 'apple_h9p_crash_resume_test'))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p/'production.h').write_text(src)
            subprocess.run(['clang', '-Werror=implicit-function-declaration', '-I', d,
                            str(HERE/'tests/phoenix-ans.c'), '-o', str(p/'test')], check=True)
            subprocess.run([str(p/'test')], check=True)

    def storage_fixture(self, supported=True, quiesce=True, blocked=False):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            for f, val in {'sys/module/pcie_apple_h9p/parameters/probe': 'N',
                           'sys/module/nvme/parameters/apple_ans_probe': 'N',
                           'sys/bus/platform/drivers_probe': ''}.items():
                q = p/f; q.parent.mkdir(parents=True, exist_ok=True); q.write_text(val)
            (p/'run').mkdir()
            host = p/'sys/bus/platform/devices/610000000.pcie'; host.mkdir(parents=True)
            if supported:
                (p/'sys/module/pcie_apple_h9p/parameters/crash_quiesce_supported').write_text('Y')
            if quiesce:
                (host/'crash_quiesce').touch()
            if blocked:
                (p/'sys/bus/platform/drivers_probe').unlink()
                import os
                os.mkfifo(p/'sys/bus/platform/drivers_probe')
            src = (HERE/'device/phoenix-storage').read_text()
            for prefix in ('/sys/', '/run/'):
                src = src.replace(prefix, str(p)+prefix)
            script = 'say() { echo "$*"; }; sleep() { /bin/sleep 0.002; };\n'+src
            script += '\nphoenix_storage; status=$?; for pid in $(jobs -p); do kill "$pid"; done; wait; exit "$status"'
            return subprocess.run(['bash', '-c', script], text=True, capture_output=True, timeout=5)

    def test_phoenix_success_legacy_busy_timeout_and_blocked_probe(self):
        self.assertEqual(self.storage_fixture().returncode, 0)
        self.assertEqual(self.storage_fixture(supported=False).returncode, 0)
        self.assertNotEqual(self.storage_fixture(quiesce=False).returncode, 0)
        self.assertNotEqual(self.storage_fixture(quiesce=False, blocked=True).returncode, 0)

    def test_pstore_no_replay_copy_idempotence_and_archive(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            for f, val in {'sys/fs/pstore/dmesg-ramoops-0': 'TEST PANIC trace\n',
                           'sys/fs/pstore/console-ramoops-0': 'PM: resume\n',
                           'proc/sys/kernel/random/boot_id': 'test-boot'}.items():
                q = p/f; q.parent.mkdir(parents=True, exist_ok=True); q.write_text(val)
            src = (HERE/'device/linux-a9-pstore').read_text()
            src = re.sub(r'/(?:sys|run|proc|var)/', lambda m: str(p)+m[0], src)
            src = src.replace('mount -t pstore', 'true -t pstore')
            def run(arg):
                return subprocess.run(['sh', '-c', src, 'pstore', arg], text=True, capture_output=True, check=True)
            output = run('--read').stdout
            self.assertIn('dmesg-ramoops-0 saved', output)
            self.assertIn('console-ramoops-0 saved', output)
            self.assertNotIn('TEST PANIC', output)
            self.assertNotIn('PM: resume', output)
            self.assertEqual(run('--read').stdout, '')
            run('--archive')
            self.assertEqual((p/'var/lib/linux-a9/pstore/test-boot/dmesg-ramoops-0').read_text(), 'TEST PANIC trace\n')
            self.assertEqual((p/'var/lib/linux-a9/pstore/test-boot/console-ramoops-0').read_text(), 'PM: resume\n')
            self.assertTrue((p/'sys/fs/pstore/dmesg-ramoops-0').exists())


if __name__ == '__main__':
    unittest.main(argv=['test-recovery-offline'], verbosity=2)
