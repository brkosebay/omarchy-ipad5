#!/usr/bin/env python3
"""Verify the real staged files against this unit's offline IOService/ADT inputs."""
import hashlib
import json
from pathlib import Path
import struct
import sys

from pull_firmware import hcd_images, live_radio_profile, parse_ioreg, sha, wifi_prefix

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / 'build/firmware'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    service = (FW / 'private/IOService.txt').read_bytes()
    chip, nvram, bt_name, evidence = live_radio_profile(service)
    selection = json.loads((FW / 'selection.json').read_text())
    require(selection['ioservice_sha256'] == sha(service), 'IOService evidence has changed; restage')
    require((chip, nvram, bt_name) == (selection['chip'], selection['nvram'], selection['bt_name']), 'selection mismatch')
    manifest = json.loads((FW / 'manifest.json').read_text())
    for item in manifest:
        data = (FW / item['path']).read_bytes()
        require(len(data) == item['size'] and sha(data) == item['sha256'], 'manifest mismatch: ' + item['path'])
    candidates = [c for c in hcd_images((FW / 'sources/usr/sbin/BlueTool').read_bytes()) if c['name'] == bt_name]
    require(candidates and len({sha(c['data']) for c in candidates}) == 1, 'missing/ambiguous HCD')
    expected = {'brcm/BCM.apple,j71t.hcd': candidates[0]['data']}
    directory = FW / 'sources/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware' / chip
    board = 'apple,feara-' + nvram.split('_M-', 1)[1].replace('_V-', '-').replace('__m-', '-')[:-4]
    for suffix, source in [('bin', 'feara.trx'), ('txt', nvram), ('clm_blob', 'feara.clmb'), ('txcap_blob', 'feara.txcb')]:
        for country in ('', '-XX'):
            expected[f'brcm/{wifi_prefix(chip)}.{board}{country}.{suffix}'] = (directory / source).read_bytes()
    nodes = parse_ioreg((FW / 'private/IODeviceTree.txt').read_bytes())
    als = [n['alsCalibration'] for n in nodes if 'alsCalibration' in n]
    require(len(als) == 1, 'missing/ambiguous ALS calibration')
    lsci = als[0]
    require(lsci == (FW / 'private/LSCI.bin').read_bytes(), 'private ALS differs from ADT')
    require(len(lsci) == 0x48 and lsci[1:4] == b'\x02\x01\x08', 'unsupported CT819 header')
    require(struct.unpack_from('<H', lsci, 4)[0] == len(lsci), 'invalid ALS length')
    require(sum(struct.unpack('<36H', lsci)) & 0xffff == 0xffff, 'invalid ALS checksum')
    record = lsci[8:]
    require(record[0] == 0 and record[3:5] == b'\x40\x01' and not any(record[0x2a:0x30]), 'unsupported CT819 record/leakage')
    require(record[0x10] in (1, 8, 16, 128), 'invalid ALS gain')
    fudge, integration = struct.unpack_from('<HH', record, 8)
    require(fudge > 0 and 50 <= integration <= 650, 'invalid ALS timing/fudge')
    require(all(0 < struct.unpack_from('<I', record, o)[0] < 2**31 for o in (0x14, 0x18, 0x1c, 0x20, 0x24)), 'invalid ALS coefficients')
    expected['apple/j71t-LSCI.bin'] = lsci
    cbat = [n['usb-input-limit-calibration'] for n in nodes
            if isinstance(n.get('usb-input-limit-calibration'), bytes)]
    require(len(cbat) == 1, 'missing/ambiguous CBAT')
    require(cbat[0] == (FW / 'private/CBAT.bin').read_bytes(), 'private CBAT differs from ADT')
    require(len(cbat[0]) == 24 and struct.unpack_from('<I', cbat[0], 20)[0] == 1,
            'unsupported CBAT record')
    expected['apple/j71t-CBAT.bin'] = cbat[0]
    bt_addresses = []
    for node in nodes:
        if node.get('IORegistryEntryName', '').split('@')[0] == 'bluetooth':
            if isinstance(node.get('local-mac-address'), bytes):
                bt_addresses.append(node['local-mac-address'])
        if isinstance(node.get('mac-address-bluetooth0'), bytes):
            bt_addresses.append(node['mac-address-bluetooth0'])
    require(bt_addresses and len(set(bt_addresses)) == 1, 'missing/ambiguous Bluetooth identity')
    bmac = bt_addresses[0]
    require(len(bmac) == 6 and any(bmac) and not bmac[0] & 1, 'invalid Bluetooth identity')
    require(bmac == (FW / 'private/BMac.bin').read_bytes(), 'private BMac differs from ADT')
    expected['apple/j71t-BMac.bin'] = bmac
    smartio = (FW / 'lib/firmware/apple/s8000-smartio.bin').read_bytes()
    require(hashlib.sha256(smartio).hexdigest() == 'da53095ed3364689ffd56f6576f036e4741a98836147a181c335626389ef5a36', 'SmartIO differs from extracted A9 section')
    expected['apple/s8000-smartio.bin'] = smartio
    actual = {str(p.relative_to(FW / 'lib/firmware')): p.read_bytes() for p in (FW / 'lib/firmware').rglob('*') if p.is_file()}
    require(all(actual.get(name) == data for name, data in expected.items()),
            'periph firmware differs from exact expected files/bytes')
    extra = sorted(set(actual) - set(expected))
    print(f'PASS: {len(expected)} firmware files match offline sources, live Wi-Fi selection and CT819 record checks')
    print(f'Wi-Fi verified: {chip}/{nvram}')
    print(f'Bluetooth inferred from combo-module vendor: {bt_name}; {len(candidates)} identical BlueTool copies')
    if extra:
        print('Additional staged files are outside peripherals verification: ' + ', '.join(extra))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, struct.error) as error:
        sys.exit(str(error))
