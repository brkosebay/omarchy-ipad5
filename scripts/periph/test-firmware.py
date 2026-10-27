#!/usr/bin/env python3
"""Offline regression tests for profile selection, HCD bounds, and calibration.

Synthetic fixtures only. Nothing here connects to a device or uses real keys.
"""
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("firmware_pull", ROOT / "scripts/periph/pull_firmware.py")
FW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FW)
NAME = "BCM4355C0 37.4 MHz Feara MUR"
PROFILE = "P-feara_M-TEST_V-m__m-1.0.txt"


def cmd(opcode, payload):
    return opcode.to_bytes(2, "little") + bytes([len(payload)]) + payload


def image(name=NAME):
    name = name.encode()
    return (cmd(0xfc4c, b"\0" * 4 + b"BRCMcfgS") +
            cmd(0xfc4c, b"\0" * 4 + b"BRCMcfgD" + b"\0" * 10 + bytes([len(name)]) + name) +
            cmd(0xfc4e, b"\0" * 4))


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "build/periph").mkdir(parents=True, exist_ok=True)
        (ROOT / "build/firmware").mkdir(parents=True, exist_ok=True)
        self.source_tmp = tempfile.TemporaryDirectory(dir=ROOT / "build/periph", prefix="test-source-")
        self.output_tmp = tempfile.TemporaryDirectory(dir=ROOT / "build/firmware", prefix="test-output-")
        self.addCleanup(self.source_tmp.cleanup)
        self.addCleanup(self.output_tmp.cleanup)
        self.source = Path(self.source_tmp.name)
        self.output = Path(self.output_tmp.name)
        self.put("usr/sbin/BlueTool", b"MachO fixture" + image())
        for name in ("feara.trx", "feara.clmb", "feara.txcb", PROFILE):
            self.put("usr/share/firmware/wifi/C-4355__s-C0/" + name, b"TEST ONLY:" + name.encode())

    def put(self, name, data):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run_pull(self, *extra):
        return subprocess.run([sys.executable, "-B", str(ROOT / "scripts/periph/pull_firmware.py"),
                               "--source", str(self.source), "--output", str(self.output), *extra],
                              capture_output=True, text=True)

    def selected(self, *extra):
        return self.run_pull("--chip", "C-4355__s-C0", "--nvram", PROFILE,
                             "--bt-name", NAME, *extra)

    def test_inventory_does_not_select_firmware(self):
        result = self.run_pull()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.output / "lib").exists())
        self.assertEqual(json.loads((self.output / "bluetooth-candidates.json").read_text())[0]["name"], NAME)

    def test_exact_profile_and_board_aliases(self):
        result = self.selected()
        self.assertEqual(result.returncode, 0, result.stderr)
        brcm = self.output / "lib/firmware/brcm"
        self.assertEqual((brcm / "BCM.apple,j71t.hcd").read_bytes(), image())
        self.assertTrue((brcm / "brcmfmac4355-pcie.apple,feara-TEST-m-1.0-XX.txt").is_file())
        self.assertEqual(len(list(brcm.iterdir())), 9)

    def test_truncated_hcd_is_not_a_candidate(self):
        self.assertEqual(list(FW.hcd_images(image()[:-1])), [])

    def test_differing_duplicate_hcd_is_rejected(self):
        second = image()[:-1] + b"\x01"
        self.put("usr/sbin/BlueTool", image() + second)
        result = self.selected()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ambiguous", result.stderr)
        self.assertFalse((self.output / "lib/firmware/brcm/BCM.apple,j71t.hcd").exists())

    def test_manufacturer_mismatch_is_rejected(self):
        name = NAME.replace("MUR", "USI")
        self.put("usr/sbin/BlueTool", image(name))
        result = self.run_pull("--chip", "C-4355__s-C0", "--nvram", PROFILE, "--bt-name", name)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manufacturer", result.stderr)

    def lsci(self, corrupt=False):
        data = bytearray(0x48)
        data[1:4] = b"\x02\x01\x08"
        data[4:6] = len(data).to_bytes(2, "little")
        checksum = (0xffff - sum(int.from_bytes(data[i:i+2], "little") for i in range(0, len(data), 2))) & 0xffff
        data[6:8] = checksum.to_bytes(2, "little")
        if corrupt:
            data[-1] ^= 1
        p = self.source / "ioreg.plist"
        p.write_bytes(plistlib.dumps([{ "alsCalibration": bytes(data), "multi-touch-calibration": b"syscfg/MtCl" }]))
        return p, bytes(data)

    def test_live_lsci_record_is_staged_without_placeholder_touch(self):
        path, data = self.lsci()
        result = self.run_pull("--ioreg", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        p = self.output / "lib/firmware/apple/j71t-LSCI.bin"
        self.assertEqual(p.read_bytes(), data)
        self.assertEqual(p.stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.output / "private/MtCl.bin").exists())

    def test_bad_lsci_checksum_is_not_staged(self):
        path, _ = self.lsci(corrupt=True)
        result = self.run_pull("--ioreg", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum", result.stderr)
        self.assertFalse((self.output / "lib/firmware/apple/j71t-LSCI.bin").exists())

    def test_dry_run_needs_no_connection(self):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/periph/pull_firmware.py"), "--dry-run"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["remote_writes"])

    def test_text_ioreg_latin1_mac_and_calibration(self):
        _, lsci = self.lsci()
        mac = bytes.fromhex('020000000001')
        text = (b'+-o Root  <class IORegistryEntry, id 1>\n'
                b'  +-o wlan  <class AppleSimpleUARTSync, id 2>\n'
                b'    "local-mac-address" = <"' + mac + b'">\n'
                b'    "wifi-calibration-msf" = "REDACTED-CALIBRATION"\n'
                b'    "rx-calibration-temp" = <00>\n'
                b'  +-o als1  <class AppleARMIODevice, id 3>\n'
                b'    "alsCalibration" = <' + lsci.hex().encode() + b'>\n'
                b'  +-o multi-touch  <class IOService, id 4>\n'
                b'    "multi-touch-calibration" = <"syscfg/MtCl">\n')
        path = self.source / 'ioreg.txt'
        path.write_bytes(text)
        result = self.run_pull('--ioreg', str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / 'private/WMac.bin').read_bytes(), mac)
        self.assertEqual((self.output / 'private/WRxT.bin').read_bytes(), b'\0')
        self.assertEqual((self.output / 'lib/firmware/apple/j71t-LSCI.bin').read_bytes(), lsci)
        self.assertFalse((self.output / 'private/MtCl.bin').exists())

    def test_text_ioreg_binary_escape_and_conflict(self):
        tree = FW.parse_ioreg(b'+-o wlan  <class IOService, id 1>\n  "local-mac-address" = <"\x85ab\\\\\\\"c">\n')
        self.assertEqual(tree[0]['local-mac-address'], b'\x85ab\\"c')
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            FW.parse_ioreg(b'+-o wlan  <class IOService, id 1>\n "x" = <01>\n "x" = <02>\n')

    def test_bluetooth_identity_is_staged_in_original_byte_order(self):
        address = bytes.fromhex('021020304050')
        path = self.source / 'ioreg.plist'
        path.write_bytes(plistlib.dumps([
            {'IORegistryEntryName': 'bluetooth', 'local-mac-address': address},
            {'mac-address-bluetooth0': address},
        ]))
        result = self.run_pull('--ioreg', str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        staged = self.output / 'lib/firmware/apple/j71t-BMac.bin'
        self.assertEqual(staged.read_bytes(), address)
        self.assertEqual(staged.stat().st_mode & 0o777, 0o600)

    def test_invalid_bluetooth_identity_is_not_staged(self):
        path = self.source / 'ioreg.plist'
        path.write_bytes(plistlib.dumps([
            {'IORegistryEntryName': 'bluetooth', 'local-mac-address': b'\x01' * 6},
        ]))
        result = self.run_pull('--ioreg', str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nonzero unicast', result.stderr)
        self.assertFalse((self.output / 'lib/firmware/apple/j71t-BMac.bin').exists())

    def ioservice(self, requested_profile=PROFILE):
        text = ('+-o wifi  <class IOService, id 1>\n'
                ' "HWIdentifiers" = {"V"="m","M"="TEST","C"=4355,"s"="C0","P"="feara","m"="1.0"}\n'
                ' "RequestedFiles" = ({"Firmware"="C-4355__s-C0/feara.trx",'
                '"TxCap"="C-4355__s-C0/feara.txcb","Regulatory"="C-4355__s-C0/feara.clmb",'
                f'"NVRAM"="C-4355__s-C0/{requested_profile}"}})\n')
        path = self.source / 'IOService.txt'
        path.write_text(text)
        return path

    def test_live_radio_selection_and_identical_feara_duplicates(self):
        name = 'BCM4355C0 FearA MUR MCC'
        self.put('usr/sbin/BlueTool', image(name) + b'\0' + image(name))
        result = self.run_pull('--ioservice', str(self.ioservice()))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / 'lib/firmware/brcm/BCM.apple,j71t.hcd').read_bytes(), image(name))
        selection = json.loads((self.output / 'selection.json').read_text())
        self.assertTrue(selection['bluetooth_claim'].startswith('inferred:'))

    def test_live_requested_files_must_match_identifiers(self):
        result = self.run_pull('--ioservice', str(self.ioservice(PROFILE.replace('1.0', '2.0'))))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('disagree', result.stderr)
        self.assertFalse((self.output / 'lib').exists())

    def test_explicit_profile_cannot_override_live_evidence(self):
        result = self.run_pull('--ioservice', str(self.ioservice()), '--chip', 'C-4355__s-C1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('disagrees', result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
