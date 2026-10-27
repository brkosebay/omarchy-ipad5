#!/usr/bin/env python3
"""Host-only checks of the packaged showcase payload (scripts/showcase/package.py):
extraction, checksums, cmdline, trimmed DTB, Image/initramfs/firmware identity
with the base AOP entry, plus rejected cmdline and DTB variants."""
import gzip
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/showcase'
PKG = runpy.run_path(str(ROOT / 'scripts/showcase/package.py'))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((OUT / 'manifest.json').read_text())
        cls.env = dict(line.split('=', 1) for line in (OUT / 'entries.env').read_text().splitlines())
        cls.entry = ROOT / cls.manifest['entry_dir']
        base = PKG['BASES'][cls.manifest['base']]
        cls.periph = ROOT / 'build/periph' / base['tree']
        cls.kernel = ROOT / f'build/kernel-{base["tree"]}'
        env = dict(line.split('=', 1) for line in (cls.periph / 'entries.env').read_text().splitlines())
        cls.base_dir = Path(env[base['key'] + '_DIR'])
        cls.base_entry = env[base['key'] + '_ENTRY']
        cls.commit = base['commit']

    def test_names_and_manifest(self):
        name = f'showcase-aop-oc-{self.commit[:10]}'
        self.assertEqual(self.manifest['entry'], name)
        self.assertEqual(self.env['SHOWCASE_ENTRY'], name)
        self.assertEqual(Path(self.env['SHOWCASE_PAYLOAD']), ROOT / self.manifest['payload'])
        self.assertEqual(Path(self.env['SHOWCASE_DIR']), self.entry)
        self.assertEqual(sha(ROOT / self.manifest['payload']), self.manifest['payload_sha256'])
        self.assertFalse(self.manifest['rebuilt'])
        self.assertEqual(self.manifest['kernel_commit'], self.commit)
        self.assertEqual((self.entry / 'COMMIT').read_text().split()[0], self.commit)
        self.assertEqual(self.env['SHOWCASE_RELEASE'], (self.entry / 'RELEASE').read_text().strip())
        self.assertFalse((self.entry / 'DIRTY').read_text().strip())
        self.assertFalse((self.entry / 'SOURCE.patch').read_bytes())

    def test_entry_checksums(self):
        lines = (self.entry / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(lines), len([p for p in self.entry.iterdir() if p.name != 'SHA256SUMS']))
        for line in lines:
            digest, name = line.split('  ', 1)
            self.assertEqual(sha(self.entry / name), digest, name)
            self.assertEqual(self.manifest['files'][name], digest)

    def test_payload_extracts_to_the_entry(self):
        with tempfile.TemporaryDirectory(prefix='showcase-payload-') as temp:
            subprocess.run(['python3', '-B', '-I', str(ROOT / 'scripts/boot/prepare-entry.py'),
                            str(ROOT / self.manifest['payload']), temp, '--board', 'apple,j71t'],
                           check=True, capture_output=True)
            for name in ('cmdline', 'dtb', 'Image.gz', 'initramfs.gz'):
                self.assertEqual((Path(temp) / name).read_bytes(), (self.entry / name).read_bytes(), name)
        payload = (ROOT / self.manifest['payload']).read_bytes()
        m1n1 = (ROOT / 'build/m1n1-boot/m1n1.bin').read_bytes()
        self.assertEqual(payload[:len(m1n1)], m1n1)
        vars_ = payload[len(m1n1):].split(b'\n', 2)
        self.assertEqual(vars_[0], b'psci=1')
        self.assertEqual(vars_[1], b'chosen.bootargs=' + (self.entry / 'cmdline').read_bytes().strip())

    def test_cmdline(self):
        text = (self.entry / 'cmdline').read_text()
        self.assertTrue(text.endswith('\n') and text.count('\n') == 1)
        text = text.strip()
        PKG['check_cmdline'](text)
        self.assertLess(len(text.encode()), 1000)
        base = (self.base_dir / 'cmdline').read_text().split()
        want = [f'linux_a9.entry={self.manifest["entry"]}' if t == f'linux_a9.entry={self.base_entry}' else t
                for t in base if t != 'systemd.debug_shell=ttyGS0']
        want += ['apple_soc_cpufreq.a9_tuning=1', 'apple_soc_cpufreq.a9_passive_millic=90000']
        self.assertEqual(text.split(), want)
        self.assertEqual(text.split().count('pcie_apple_h9p.ans_keep_power=1'), 1)
        self.assertNotIn('debug_shell', text)

    def test_rejected_cmdlines(self):
        good = (self.entry / 'cmdline').read_text().strip()
        for bad in (good + ' systemd.debug_shell=ttyGS0', good + ' pcie_apple_h9p.ans_keep_power=1',
                    good + ' pcie_apple_h9p.ans_keep_power=0',
                    good.replace(' apple_soc_cpufreq.a9_tuning=1', ''),
                    good.replace('apple_soc_cpufreq.a9_passive_millic=90000', 'apple_soc_cpufreq.a9_passive_millic=92000'),
                    good.replace(' systemd.mask=maintainer@example.invalid', ''),
                    good.replace(' aop_spi0_reset=1', ''), good + ' x' * 20):
            with self.subTest(bad=bad[-60:]), self.assertRaises(AssertionError):
                PKG['check_cmdline'](bad)

    def test_trimmed_dtb(self):
        dtb = self.entry / 'dtb'
        self.assertEqual(dtb.read_bytes(), (ROOT / 'build/kernel-showcase/dtbs/s8003-j71t-showcase.dtb').read_bytes())
        removed = PKG['check_trimmed'](self.kernel / 'dtbs/s8003-j71t.dtb', dtb)
        self.assertEqual(len(removed), self.manifest['dtb_change']['removed_opp_nodes'])
        nodes = PKG['opp_nodes'](dtb)
        self.assertEqual(sorted(v for v in nodes.values()),
                         sorted([(hz, i + 1, False) for i, hz in enumerate(PKG['STOCK_HZ'])] +
                                [(2160000000, 14, True)]))
        runpy.run_path(str(ROOT / 'scripts/periph/aop/check-stage23-memory.py'))['check'](dtb.read_bytes())
        # The other three payload DTBs are the base ones, untouched.
        for name in PKG['OTHER_DTBS']:
            self.assertEqual((ROOT / 'build/kernel-showcase/dtbs' / name).read_bytes(),
                             (self.kernel / 'dtbs' / name).read_bytes())

    def test_rejected_dtbs(self):
        base = self.kernel / 'dtbs/s8003-j71t.dtb'
        with tempfile.TemporaryDirectory(prefix='showcase-dtb-') as temp:
            cases = {
                'untrimmed': [],
                'stock opp removed': ['/opp-table/opp07'],
                '2160 removed': ['/opp-table/opp14'],
                'outside the table': ['/pmu'],
            }
            removed = [f'/opp-table/{n}' for n, v in PKG['opp_nodes'](base).items() if not PKG['keep'](v)]
            for label, extra in cases.items():
                bad = Path(temp) / f'{label.replace(" ", "-")}.dtb'
                shutil.copyfile(base, bad)
                nodes = ([] if label == 'untrimmed' else removed) + extra
                if nodes:
                    subprocess.run(['fdtput', '-r', str(bad), *nodes], check=True)
                with self.subTest(label), self.assertRaises(AssertionError):
                    PKG['check_trimmed'](base, bad)
            # A kept OPP left with a different level is not the 2160 MHz slot 14.
            bad = Path(temp) / 'level.dtb'
            shutil.copyfile(base, bad)
            subprocess.run(['fdtput', '-r', str(bad), *removed], check=True)
            subprocess.run(['fdtput', '-t', 'u', str(bad), '/opp-table/opp14', 'opp-level', '15'], check=True)
            with self.assertRaises(AssertionError):
                PKG['check_trimmed'](base, bad)

    def test_image_initramfs_and_firmware_are_the_base(self):
        image = gzip.decompress((self.entry / 'Image.gz').read_bytes())
        self.assertEqual(image, gzip.decompress((self.base_dir / 'Image.gz').read_bytes()))
        for marker in PKG['IMAGE_MARKERS']:
            self.assertIn(marker, image)
        self.assertIn(self.manifest['release'].encode(), image)
        initrd = gzip.decompress((self.entry / 'initramfs.gz').read_bytes())
        self.assertEqual(initrd, gzip.decompress((self.base_dir / 'initramfs.gz').read_bytes()))
        newc = runpy.run_path(str(ROOT / 'scripts/periph/mkinitramfs.py'))['read_newc']
        content = {name: body for _, name, _, body, _ in newc(initrd)[1]}
        firmware = ROOT / self.manifest['root_firmware']
        self.assertEqual(Path(self.env['SHOWCASE_ROOT_FIRMWARE']), firmware)
        for line in (firmware / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ', 1)
            self.assertEqual(sha(firmware / name), digest)
            self.assertEqual(hashlib.sha256(content['lib/firmware/apple/aop/' + name]).hexdigest(), digest)


if __name__ == '__main__':
    raise SystemExit(not unittest.main(exit=False, verbosity=1).result.wasSuccessful())
