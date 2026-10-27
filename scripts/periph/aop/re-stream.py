#!/usr/bin/env python3
"""Pinned, host-only investigation of the stage3 calibration-state report gate.

Default exports iOS evidence and the latest capture fixture. --firmware-gate
executes the real Thumb setter and report predicate in Unicorn, with only the
final rate-policy call stubbed; no hardware, firmware file or device is changed.
Firmware decompilations are reproduced with re-calibration.py --function.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
CAL = runpy.run_path(str(Path(__file__).with_name('re-calibration.py')))
OUT = ROOT / 'build/periph/aop-stage3/re/stream'
CAPTURE = ROOT / 'build/periph/aop-stage23/stage3-caloptional-full-20261025T182236Z'
HASHES = {
    'rings.txt': 'ca8711da7e35831b45670af3a9431347ef3bbae7dff4c0bb59784722f14d167e',
    'transcript.txt': '44d43f65bf484483758b5b4e43691d23c5c4bbbbcc01d330ffa2e7c97ddb2ff3',
    'cold_after.bin': '1ab4653099481a3493f502b40880dc11cde7f84b5e0b2cfa4ac94cc123edb870',
}
IOS = {
    'calibration': 'fffffff005b9d9e4',
    'cal-complete': 'fffffff005ba0c34',
    'rates': 'fffffff005b99ef0',
    'power': 'fffffff005b9e6d8',
    'dispatch': 'fffffff005bacf58',
    'motion': 'fffffff005b9eb5c',
}


def captured():
    for name, digest in HASHES.items():
        assert hashlib.sha256((CAPTURE / name).read_bytes()).hexdigest() == digest, name
    rings, current = {}, None
    for line in (CAPTURE / 'rings.txt').read_text().splitlines():
        m = re.fullmatch(r'ep (\d+) ring (tx|rx|hist) size=0x[0-9a-f]+ valid=1', line)
        if m:
            current = dict(data=bytearray(), pointers=[])
            rings[int(m[1]), m[2]] = current
        elif current is not None:
            m = re.fullmatch(r'  \+(40|80) ([0-9a-f]{8}) .*', line)
            if m:
                current['pointers'].append(int(m[2], 16))
            m = re.fullmatch(r'  entries from 0x([0-9a-f]+):', line)
            if m:
                current['offset'] = int(m[1], 16)
            m = re.fullmatch(r'  ([0-9a-f]{8}): ((?:[0-9a-f]{8} ?){4})', line)
            if m:
                assert len(current['data']) == int(m[1], 16)
                current['data'] += struct.pack('<4I', *(int(x, 16) for x in m[2].split()))
    tx, rx = rings[33, 'tx'], rings[33, 'rx']
    assert (tx['offset'], rx['offset']) == (0x280, 0x140)
    assert tx['pointers'] == [0x380, 0x380] and rx['pointers'] == [0x240, 0x240]
    requests = [bytes(tx['data'][i:i + 128]) for i in (0, 128)]
    for tag, key, value, entry in zip((6, 7), (0x5c, 0), (0, 100000), requests):
        assert struct.unpack_from('<4I', entry) == (0x20504f41, 52, 0x10000007, 0)
        assert struct.unpack_from('<IBBHH', entry, 32) == (12, 2, 0x10, 4, tag)
        assert struct.unpack_from('<3I', entry, 56) == (0, key, value)
    replies = [bytes(rx['data'][i:i + 64]) for i in range(0, 256, 64)]
    for tag, entry in enumerate(replies, 4):
        assert struct.unpack_from('<4I', entry) == (0x20504f41, 44, 0x10000007, 4)
        assert struct.unpack_from('<IBBHH', entry, 32) == (4, 2, 0x20, 0x84, tag)
        assert struct.unpack_from('<I', entry, 56)[0] == 0
    times = {'T': [], 'R': []}
    for line in (CAPTURE / 'transcript.txt').read_text().splitlines():
        m = re.fullmatch(r'\d+ (\d+)\.(\d{9}) ([RT]) ep=0x21 (a2|85)000000000000 0 phase=(configure|wait-report)', line)
        if m:
            times[m[3]].append(int(m[1]) * 10**9 + int(m[2]))
    assert len(times['T']) == len(times['R']) == 7
    delays = [r - t for t, r in zip(times['T'], times['R'])]
    assert all(0 < d < 10**7 for d in delays)
    return requests, replies, delays


def fixture():
    requests, replies, delays = captured()
    lines = ['/* 20261025T182236Z: TX6/7, RX4..7; windows start at 280/140. */']
    for name, entries, size in (('stream_tx', requests, 128), ('stream_rx', replies, 64)):
        lines.append(f'static const u8 {name}[][{size}] = {{')
        lines += ['{' + ','.join(f'0x{b:02x}' for b in entry) + '},' for entry in entries]
        lines.append('};')
    lines.append('static const u64 stream_delay_ns[] = {' + ','.join(f'{d}ULL' for d in delays) + '};')
    return '\n'.join(lines) + '\n'


def firmware_gate():
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
    from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == CAL['FW_SHA']
    frozen = (CAPTURE / 'cold_after.bin').read_bytes()
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x1000000, 0xa0000)
    u.mem_write(0x1000000, frozen)
    assert u.mem_read(0x100d31c, 0x46) == fw[0xd31c:0xd362]
    # Isolate this predicate from rate policy (the final call after all gates).
    u.mem_write(0x100a8bc, bytes.fromhex('00207047'))
    u.mem_map(0x2000000, 0x1000)
    u.mem_map(0x3000000, 0x2000)
    source, payload = 0x1067370, 0x3000000
    def put(address, value):
        u.mem_write(address, struct.pack('<I', value))
    def call(address, args):
        u.reg_write(UC_ARM_REG_SP, 0x3001000)
        put(0x3001000, 0)  # setter's fifth argument: caller identity, unused for 3d
        u.reg_write(UC_ARM_REG_LR, 0x2000001)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            u.reg_write(reg, value)
        u.emu_start(address | 1, 0x2000000, count=1000)
        assert u.reg_read(UC_ARM_REG_PC) == 0x2000000
        return u.reg_read(UC_ARM_REG_R0)
    override = struct.unpack_from('<I', fw, 0xd364)[0]
    assert override == 0x106d244 and u.mem_read(override, 1) == b'\0'
    assert u.mem_read(source + 0x50, 4) == b'\0' * 4
    put(source + 0x68, 1)  # active sampling, timestamps already expired
    for offset in (0x40, 0x44, 0x48, 0x4c, 0xa4):
        put(source + offset, 0)
    before = call(0x100d31c, (source, 0, 1, 0))
    values = []
    for state in (1, 2, 0):
        put(payload, state)
        assert call(0x100c8ac, (source, 0x3d, payload, 4)) == 0
        assert u.mem_read(source + 0x50, 4) == struct.pack('<I', state)
        allowed = call(0x100d31c, (source, 0, 1, 0))
        assert allowed == bool(state)
        values.append(dict(calibration_state=state, report_allowed=allowed))
    assert before == 0
    result = dict(before=before, after=values, override_address=hex(override), override_value=0,
                  real_code=['0100c8ac property setter', '0100d31c report predicate'],
                  stub='0100a8bc final rate policy returns 0; active/timestamps modeled; no sensor/MMIO emulation')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'firmware-gate.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS actual Thumb firmware: active accel + state0 suppresses reports; property3d=1/2 opens report gate; property3d=0 closes it')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--firmware-gate', action='store_true')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    requests, replies, delays = captured()
    for name, address in IOS.items():
        path = OUT / (name + '.json')
        if args.refresh or not path.exists():
            path.write_text(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', f'address={address}'], text=True))
        result = json.loads(path.read_text())
        assert result['code'] and not result['truncated']
        (OUT / (name + '.c')).write_text(result['code'])
    (OUT / 'capture-fixture.h').write_text(fixture())
    (OUT / 'capture-fixture.h').chmod(0o600)
    evidence = dict(captures=HASHES, firmware_sha256=CAL['FW_SHA'], kernelcache_sha256=CAL['KC_SHA'],
        queues=dict(tx=[0x380, 0x380], rx=[0x240, 0x240], unread=0),
        commands=['1a', '0b', '18 (unsupported)', '19', '16=100000', '5c=0', '0=100000'],
        captured_rx_tags=[4, 5, 6, 7], captured_tx_tags=[6, 7], reply_delays_ns=delays,
        missing='3d=2 after complete calibration, before rates; 3d=1 if incomplete',
        device_success=False)
    (OUT / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    if args.firmware_gate:
        firmware_gate()
    print(f'PASS captured seven replies, drained queues, nonzero report interval; iOS calibration-state/report path: {OUT}')


if __name__ == '__main__':
    main()
