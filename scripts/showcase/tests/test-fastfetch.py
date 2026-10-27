#!/usr/bin/env python3
"""Host-only test of the iPad fastfetch Wi-Fi/Bluetooth line
(scripts/omarchy/rootfs/tablet/fastfetch.jsonc): the real command text, run by
sh and bash against a fake sysfs and stub iw/nmcli/bluetoothctl/timeout."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CONFIG = ROOT / 'scripts/omarchy/rootfs/tablet/fastfetch.jsonc'
SSID = 'REDACTED'
IW_5G = f'Connected to 00:00:00:00:00:00 (on wlan0)\n\tSSID: {SSID}\n\tfreq: 5180.0\n\tsignal: -52 dBm\n'
IW_24G = f'Connected to 00:00:00:00:00:00 (on wlan0)\n\tSSID: {SSID}\n\tfreq: 2437\n'
IW_6G = f'Connected to 00:00:00:00:00:00 (on wlan0)\n\tSSID: {SSID}\n\tfreq: 5955.0\n'
NMCLI = ' :2412 MHz\n*:5745 MHz\n :5180 MHz\n'


def modules():
    text = '\n'.join(line for line in CONFIG.read_text().splitlines() if not line.lstrip().startswith('//'))
    return json.loads(text)['modules']


def network_command():
    found = [m['text'] for m in modules() if isinstance(m, dict) and 'Bluetooth' in m.get('text', '')]
    assert len(found) == 1
    return found[0]


class WifiLineTests(unittest.TestCase):
    shells = [s for s in ('sh', 'bash', 'dash') if shutil.which(s)]

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.sys = self.tmp / 'sys'
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        self.stub('timeout', 'shift; exec "$@"')
        lo = self.sys / 'class/net/lo'
        lo.mkdir(parents=True)
        (lo / 'flags').write_text('0x9\n')
        (lo / 'operstate').write_text('unknown\n')
        self.wlan = self.sys / 'class/net/wlan0'
        self.wlan.mkdir()
        (self.wlan / 'phy80211').mkdir()
        self.set_link(flags='0x1003', operstate='up')

    def stub(self, name, body):
        path = self.bin / name
        path.write_text(f'#!/bin/sh\n{body}\n')
        path.chmod(0o755)

    def stub_output(self, name, text, status=0):
        (self.tmp / f'{name}.out').write_text(text)
        self.stub(name, f'cat "{self.tmp}/{name}.out"; exit {status}')

    def set_link(self, flags, operstate):
        (self.wlan / 'flags').write_text(flags + '\n')
        (self.wlan / 'operstate').write_text(operstate + '\n')

    def line(self):
        outputs = set()
        for shell in self.shells:
            env = dict(PATH=f'{self.bin}:/usr/bin:/bin', LINUX_A9_SYSFS=str(self.sys), LC_ALL='C')
            done = subprocess.run([shell, '-c', network_command()], env=env, capture_output=True,
                                  text=True, timeout=20)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(done.stderr, '')
            outputs.add(done.stdout)
        self.assertEqual(len(outputs), 1, outputs)
        out = outputs.pop()
        self.assertNotIn(SSID, out)
        self.assertNotIn('11:22:33', out)
        return out.rstrip('\n')

    def test_no_ssid_anywhere_in_the_command(self):
        self.assertNotIn('ssid', network_command().lower())

    def test_5ghz_from_iw(self):
        self.stub_output('iw', IW_5G)
        self.assertEqual(self.line(), 'Wi-Fi 5 GHz connected · Bluetooth none connected')

    def test_24ghz_from_iw(self):
        self.stub_output('iw', IW_24G)
        self.assertEqual(self.line(), 'Wi-Fi 2.4 GHz connected · Bluetooth none connected')

    def test_6ghz_from_iw(self):
        self.stub_output('iw', IW_6G)
        self.assertEqual(self.line(), 'Wi-Fi 6 GHz connected · Bluetooth none connected')

    def test_band_from_nmcli_without_rescan_when_iw_is_missing(self):
        self.stub('nmcli', f'echo "$@" > "{self.tmp}/nmcli.args"; printf "{NMCLI.encode("unicode_escape").decode()}"')
        self.assertEqual(self.line(), 'Wi-Fi 5 GHz connected · Bluetooth none connected')
        args = (self.tmp / 'nmcli.args').read_text().split()
        self.assertEqual(args[args.index('--rescan') + 1], 'no')
        self.assertIn('ifname', args)

    def test_connected_without_band_tools(self):
        self.assertEqual(self.line(), 'Wi-Fi connected · Bluetooth none connected')

    def test_slow_or_failing_nmcli_still_says_connected(self):
        self.stub('nmcli', 'exit 124')
        self.assertEqual(self.line(), 'Wi-Fi connected · Bluetooth none connected')

    def test_iw_link_counts_when_operstate_is_unknown(self):
        self.set_link('0x1003', 'unknown')
        self.stub_output('iw', IW_5G)
        self.assertEqual(self.line(), 'Wi-Fi 5 GHz connected · Bluetooth none connected')

    def test_up_but_not_associated(self):
        self.set_link('0x1003', 'dormant')
        self.stub_output('iw', 'Not connected.\n', 1)
        self.assertEqual(self.line(), 'Wi-Fi not connected · Bluetooth none connected')

    def test_interface_down_is_off(self):
        self.set_link('0x1002', 'down')
        self.stub_output('iw', IW_5G)
        self.assertEqual(self.line(), 'Wi-Fi off · Bluetooth none connected')

    def test_no_wireless_interface_is_off(self):
        shutil.rmtree(self.wlan)
        self.assertEqual(self.line(), 'Wi-Fi off · Bluetooth none connected')

    def test_bluetooth_part_is_kept(self):
        self.stub_output('iw', IW_5G)
        self.stub_output('bluetoothctl', 'Device 00:00:00:00:00:00 Magic Keyboard\nDevice 00:00:00:00:00:00 Pencil\n')
        self.assertEqual(self.line(), 'Wi-Fi 5 GHz connected · Bluetooth Magic Keyboard,Pencil')

    def cpu_line(self):
        cpu = [m['text'] for m in modules() if isinstance(m, dict) and 'Twister' in m.get('text', '')]
        self.assertEqual(len(cpu), 1)
        self.assertNotIn('cpuinfo_max_freq', cpu[0])
        env = dict(PATH='/usr/bin:/bin', LINUX_A9_SYSFS=str(self.sys))
        return {subprocess.run([s, '-c', cpu[0]], env=env, capture_output=True, text=True, check=True).stdout
                for s in self.shells}

    def test_cpu_line_shows_the_effective_cap(self):
        policy = self.sys / 'devices/system/cpu/cpu0/cpufreq'
        policy.mkdir(parents=True)
        (policy / 'cpuinfo_max_freq').write_text('2160000\n')
        (policy / 'scaling_max_freq').write_text('2160000\n')
        self.assertEqual(self.cpu_line(), {'Apple A9 (S8003), 2x Twister @ 2160 MHz\n'})
        # After a guard revert the QoS cap is stock again; cpuinfo stays 2160000.
        (policy / 'scaling_max_freq').write_text('1800000\n')
        self.assertEqual(self.cpu_line(), {'Apple A9 (S8003), 2x Twister @ 1800 MHz\n'})
        shutil.rmtree(policy)
        self.assertEqual(self.cpu_line(), {'Apple A9 (S8003), 2x Twister\n'})


if __name__ == '__main__':
    os.environ.setdefault('PYTHONDONTWRITEBYTECODE', '1')
    raise SystemExit(not unittest.main(exit=False, verbosity=1).result.wasSuccessful())
