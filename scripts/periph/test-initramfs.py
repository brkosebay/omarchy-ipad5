#!/usr/bin/env python3
"""Regression check for m1n1 replacing the initrd at each gzip member."""
import gzip
import importlib.util
from pathlib import Path
import stat
import unittest

spec = importlib.util.spec_from_file_location('mkinitramfs', Path(__file__).with_name('mkinitramfs.py'))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)


class PayloadArchive(unittest.TestCase):
    def test_single_member_retains_init_and_firmware(self):
        base, staged, modes, _ = mk.inputs()
        raw = gzip.decompress(base) + mk.overlay(staged, modes)
        image = gzip.compress(raw, mtime=0)
        mk.verify(image, base, staged, modes)
        _, entries = mk.read_newc(gzip.decompress(image))
        files = {name: body for _, name, mode, body, _ in entries if stat.S_ISREG(mode)}
        self.assertTrue(files['init'])
        self.assertTrue(files['extract.cpio.lzma'])
        self.assertEqual(files['lib/firmware/brcm/BCM.apple,j71t.hcd'],
                         staged['lib/firmware/brcm/BCM.apple,j71t.hcd'])

    def test_old_multiple_member_payload_is_rejected(self):
        base, staged, modes, _ = mk.inputs()
        broken = base + gzip.compress(mk.overlay(staged, modes), mtime=0)
        # This is exactly what m1n1 hands to the kernel after replacement.
        _, last = mk.read_newc(gzip.decompress(broken[len(base):]))
        self.assertNotIn('init', [name for _, name, _, _, _ in last])
        with self.assertRaisesRegex(ValueError, 'one complete gzip member'):
            mk.verify(broken, base, staged, modes)


if __name__ == '__main__':
    unittest.main()
