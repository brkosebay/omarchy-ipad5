#!/usr/bin/env python3
"""Offline Round 7: compile production functions, inject faults, never use hardware."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path('/work')
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path('/src/wt/boot'))
args, remaining = parser.parse_known_args()
KERNEL = args.source


def function(path, name):
    s = (KERNEL / path).read_text()
    m = re.search(r'^(?:static )?(?:[\w *]+)\b' + name + r'\(', s, re.M)
    assert m, name
    return s[m.start():s.index('\n}', m.start()) + 2]


class ProductionTests(unittest.TestCase):
    def compile_run(self, fixture, functions, prelude=''):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / 'production.h').write_text(prelude + '\n\n'.join(function(p, n) for p, n in functions))
            subprocess.run(['clang', '-std=gnu11', '-g', '-O1', '-Wall',
                            '-Wno-unused-function', '-Wno-unused-but-set-variable',
                            '-Werror=implicit-function-declaration',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I', str(d), str(ROOT / 'scripts/boot/tests' / fixture),
                            '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True, timeout=10)

    def test_nvmmu_reset_cache_poison_and_fifo(self):
        p = 'drivers/pci/controller/pcie-apple-h9p.c'
        self.compile_run('ans-io-nvmmu.c', [(p, n) for n in (
            'apple_h9p_nvmmu_program', 'apple_h9p_nvmmu_invalidate',
            'apple_ans_nvmmu_map', 'apple_ans_nvmmu_reset')])

    def test_controller_stop_before_dma_and_failed_slot_quarantine(self):
        p = 'drivers/nvme/host/pci-apple-ans.c'
        self.compile_run('ans-io-controller.c', [(p, n) for n in (
            'nvme_pci_apple_ans_reset_dma', 'nvme_pci_apple_ans_preinit',
            'nvme_pci_apple_ans_read_csts', 'nvme_pci_apple_ans_free_req')])

    def test_timeout_cq_poll_recovery_budget_and_terminal_states(self):
        p = 'drivers/nvme/host/pci.c'
        self.compile_run('ans-io-timeout.c', [(p, 'nvme_timeout')])

    def test_shared_vector_services_every_cq_and_timeout_drains_all(self):
        p = 'drivers/nvme/host/pci.c'
        text = (KERNEL / p).read_text()
        if 'nvme_apple_ans_irq_vector' not in text:
            self.skipTest('tree predates the shared-vector fix')
        names = ['nvme_cqe_pending', 'nvme_update_cq_head', 'nvme_poll_cq',
                 'nvme_apple_ans_shared_vector', 'nvme_apple_ans_vector_queue',
                 'nvme_apple_ans_irq_vector', 'nvme_irq', 'nvme_apple_ans_drain_vector']
        prelude = ''
        if 'nvme_apple_ans_irq_rescue_work' in text:
            names += ['nvme_apple_ans_irq_rescue_work', 'nvme_apple_ans_irq_rescue_start']
            prelude = '#define HAVE_RESCUE 1\n'
        self.compile_run('ans-io-shared-vector.c', [(p, n) for n in names], prelude)
        self.assertIn('nvme_apple_ans_drain_vector(nvmeq, "timeout")', function(p, 'nvme_timeout'))

    def test_slot_owner_and_local_retirement_errors_never_reset(self):
        p = 'drivers/nvme/host/pci-apple-ans.c'
        self.compile_run('ans-slot-owner.c', [(p, n) for n in (
            'nvme_pci_apple_ans_alloc_req', 'nvme_pci_apple_ans_free_req',
            'nvme_pci_apple_ans_unmap_data', 'nvme_pci_apple_ans_map_data')])

    def test_port_owner_uses_topology_without_config_register_reads(self):
        p = 'drivers/pci/controller/pcie-apple-h9p.c'
        self.compile_run('ans-port-owner.c', [(p, n) for n in (
            'apple_h9p_pcie_bus_port', 'apple_h9p_pcie_device_port')])

    def test_sysfs_host_endpoint_busy_and_fault_modes(self):
        p = 'drivers/nvme/host/pci.c'
        self.compile_run('ans-io-diag.c', [(p, 'nvme_apple_ans_dump'), (p, 'apple_ans_diag_store')])

    def test_host_dump_never_reads_invalidate_command(self):
        dump = function('drivers/pci/controller/pcie-apple-h9p.c', 'apple_ans_host_dump')
        self.assertNotIn('H9P_NVMMU_TCB_CTRL', dump)
        dump = function('drivers/nvme/host/pci.c', 'nvme_apple_ans_dump')
        self.assertNotIn('readl(', dump)
        self.assertNotIn('pci_read_config', dump)
        self.assertNotIn('apple_ans_dart_walk', dump)

    def test_rescue_dump_defaults_to_host_and_never_opens_block_devices(self):
        script = (ROOT / 'scripts/boot/device/linux-a9-ans-state').read_text()
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            fixtures = {
                'proc/sys/kernel/random/boot_id': 'fixture-boot',
                'proc/uptime': '42 0', 'proc/cmdline': 'fixture',
                'proc/interrupts': '19: 6 0 ANS',
                'sys/class/nvme/nvme0/state': 'live',
                'sys/class/nvme/nvme0/apple_ans_diag': '',
                'sys/block/nvme0n1/stat': '1 0 1 0',
                'sys/block/nvme0n1/queue/io_timeout': '5000',
            }
            for path, text in fixtures.items():
                f = d / path
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
            script = re.sub(r'/(proc|sys)/', lambda m: str(d) + m[0], script)
            # Run the whole helper, substituting only the dmesg producer.
            script = script.replace('dmesg | tail -200', 'echo fixture-kmsg | tail -200')
            for argument, expected in [('', 'host'), ('--endpoint', 'endpoint')]:
                r = subprocess.run(['sh', '-c', script, 'fixture', argument],
                                   text=True, capture_output=True, check=True)
                self.assertIn('fixture-boot', r.stdout)
                self.assertIn('19: 6 0 ANS', r.stdout)
                self.assertEqual((d / 'sys/class/nvme/nvme0/apple_ans_diag').read_text().strip(), expected)
            self.assertNotIn('/dev/nvme', script)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + remaining, verbosity=2)
