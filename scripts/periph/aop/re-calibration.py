#!/usr/bin/env python3
"""Host-only, pinned AOP calibration investigation; never accesses the device.

Firmware analysis reuses the private receiver Ghidra project, never the shared
iOS kernelcache. Extra --function/--window exports support focused follow-up.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BASE = 0x1000000
FW_SHA = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
EXT_SHA = '2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22'
KC_SHA = 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
OUT = ROOT / 'build/periph/aop-stage3/re/calibration'
CAPTURE = ROOT / 'build/periph/aop-stage23/stage3-propertyfix-full-cold-20261025T175311Z'
CAPTURE_SHA = {
    'rings.txt': '188c028b6173fa468847e2677fd2e3d17111e6a3d97aded276e93f70a9c7e378',
    'transcript.txt': '935a94e921db0f81a29fd07046b209c6f1583cc24ad6f89163eac2fbd27738ed',
}
IOS = {
    'ios-cal-value': 'fffffff005b96aec',
    'ios-cal-entry': 'fffffff005b96b60',
    'ios-cal-table': 'fffffff005b96d70',
    'ios-cal-complete': 'fffffff005ba0c34',
    'ios-calibration': 'fffffff005b9d9e4',
}
FW_FUNCTIONS = {
    0x1006a14: 'aop_accel_setProperty',
    0x10091ec: 'aop_motion_setProperty',
    0x100a918: 'aop_combo_setProperty',
    0x100aa40: 'aop_comboAccel_setProperty',
    0x100c8ac: 'aop_source_setProperty',
    0x100cd44: 'aop_source_setReportInterval',
    0x100d31c: 'aop_source_reportEligible',
    0x1006d58: 'aop_accel_processSample',
    0x10093d0: 'aop_motion_postReport',
    0x100b544: 'aop_service_postEPICReport',
    0x1011d1c: 'aop_spu_buildReply',
    0x10703d8: 'aop_motion_sensitivityCalibration',
    0x10706ac: 'aop_motion_lowTemperatureCalibration',
    0x1070ab8: 'aop_combo_interruptCalibration',
}


def captured_properties():
    """Decode the actual ring windows, whose offsets are NOT always zero."""
    for name, sha in CAPTURE_SHA.items():
        assert hashlib.sha256((CAPTURE / name).read_bytes()).hexdigest() == sha, name
    rings = {}
    current = None
    for line in (CAPTURE / 'rings.txt').read_text().splitlines():
        m = re.fullmatch(r'ep (\d+) ring (tx|rx|hist) size=0x[0-9a-f]+ valid=1', line)
        if m:
            current = dict(data=bytearray())
            rings[int(m[1]), m[2]] = current
        elif current is not None:
            m = re.fullmatch(r'  entries from 0x([0-9a-f]+):', line)
            if m:
                current['offset'] = int(m[1], 16)
            m = re.fullmatch(r'  ([0-9a-f]{8}): ((?:[0-9a-f]{8} ?){4})', line)
            if m:
                assert len(current['data']) == int(m[1], 16)
                current['data'] += struct.pack('<4I', *(int(x, 16) for x in m[2].split()))
    tx, rx = rings[33, 'tx'], rings[33, 'rx']
    assert (tx['offset'], rx['offset']) == (0x80, 0x40)
    replies = [bytes(rx['data'][i:i + 64]) for i in (0x40, 0x80, 0xc0)]
    for tag, reply in enumerate(replies, 1):
        assert struct.unpack_from('<4I', reply) == (0x20504f41, 44, 0x10000007, 4)
        assert struct.unpack_from('<IBBHH', reply, 32) == (4, 2, 0x20, 0x84, tag)
        assert struct.unpack_from('<I', reply, 56)[0] == (0xe00002c7 if tag == 3 else 0)
    requests = [bytes(tx['data'][i:i + 128]) for i in (0, 128)]
    cal = (ROOT / 'build/periph/aop-stage3/root-firmware/accel-calibration.bin').read_bytes()
    entries, at = {}, 12
    for _ in range(struct.unpack_from('<I', cal, 8)[0]):
        key, size = struct.unpack_from('<II', cal, at)
        entries[key] = cal[at + 8:at + 8 + size]
        at += 8 + size
    assert at == len(cal) and list(entries) == [0x1a, 0x0b, 0x18, 0x19]
    for tag, key, request in zip((2, 3), (0x0b, 0x18), requests):
        size = len(entries[key])
        assert struct.unpack_from('<4I', request) == (0x20504f41, 48 + size, 0x10000007, 0)
        assert struct.unpack_from('<IBBHH', request, 32) == (8 + size, 2, 0x10, 4, tag)
        assert request[56:64] == struct.pack('<II', 0, key)
        assert request[64:64 + size] == entries[key]
    times = {'T': [], 'R': []}
    for line in (CAPTURE / 'transcript.txt').read_text().splitlines():
        m = re.fullmatch(r'(\d+) (\d+)\.(\d{9}) ([RT]) ep=0x21 ([0-9a-f]{14}) 0 phase=configure', line)
        if m:
            expected = 'a2000000000000' if m[4] == 'T' else '85000000000000'
            if m[5] == expected:
                times[m[4]].append(int(m[2]) * 10**9 + int(m[3]))
    assert len(times['T']) == len(times['R']) == 3
    delays = [r - t for t, r in zip(times['T'], times['R'])]
    assert all(0 < d < 10**7 for d in delays)
    return requests, replies, delays, entries


def fixture():
    requests, replies, delays, _ = captured_properties()
    lines = ['/* DFU cold capture 20261025T175311Z: actual TX2/TX3 and RX1..3. */']
    for name, data, size in (('cold_cal_tx', requests, 128), ('cold_cal_rx', replies, 64)):
        lines.append(f'static const u8 {name}[][{size}] = {{')
        lines += ['{' + ','.join(f'0x{b:02x}' for b in entry) + '},' for entry in data]
        lines.append('};')
    lines.append('static const u64 cold_cal_delay_ns[] = {' + ','.join(f'{d}ULL' for d in delays) + '};')
    return '\n'.join(lines) + '\n'


def evidence(refresh=False):
    requests, replies, delays, entries = captured_properties()
    OUT.mkdir(parents=True, exist_ok=True)
    for name, address in IOS.items():
        path = OUT / (name + '.json')
        if refresh or not path.exists():
            path.write_text(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', f'address={address}'], text=True))
        result = json.loads(path.read_text())
        assert result['code'] and not result['truncated']
        (OUT / (name + '.c')).write_text(result['code'])
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    ext = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop_ext-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == FW_SHA and hashlib.sha256(ext).hexdigest() == EXT_SHA
    kc = ROOT / 'build/kc/20H350__iPad6,11/kernelcache.release.iPad6,11_12'
    assert hashlib.sha256(kc.read_bytes()).hexdigest() == KC_SHA
    def word(address):
        return struct.unpack_from('<I', fw, address - BASE)[0]
    # Immutable service descriptor + HELLO-time object construction. This is
    # not a failure-time PC trace or a claim that the snapshot sampled a call.
    assert word(0x10533c4) == 0x103963d and word(0x10533d0) == 0x1067370
    assert fw[0x3963d:0x39643] == b'accel\0'
    frozen = (CAPTURE / 'cold_after.bin').read_bytes()
    assert struct.unpack_from('<I', frozen, 0x67370)[0] == 0x10361f0
    assert word(0x1036210) == 0x100aa41  # accel service setter at vtable+20
    assert word(0x1036118) == 0x100a919  # combo setter at vtable+20
    assert fw[0x9208:0x920c].hex() == '182939d0'  # cmp key,18; beq return constant
    assert struct.unpack_from('<I', fw, 0x92b0)[0] == 0xe00002c7
    # Combo setter 100a918 intercepts 19 BEFORE the generic accel handler.
    assert fw[0xa918:0xa91c].hex() == '192907d0'
    lengths = struct.unpack_from('<5I', fw, 0x361d4)
    assert lengths[3] * 4 == len(entries[0x19]) == 16
    version, accel_period, clock, gyro_period = struct.unpack('<4I', entries[0x19])
    assert version == 3
    low_bias, span, lower, upper = struct.unpack_from('<4I', ext, 0x70b88 - 0x6e000)
    assert ((clock * accel_period + low_bias) & 0xffffffff) <= span
    assert lower <= clock * gyro_period <= upper
    report = dict(firmware_sha256=FW_SHA, ext_sha256=EXT_SHA, kernelcache_sha256=KC_SHA,
        captures=CAPTURE_SHA,
        calibration=[dict(key=hex(k), size=len(v)) for k, v in entries.items()],
        captured_statuses=[0, 0, '0xe00002c7 (kIOReturnUnsupported)'],
        wire_reply_delays_ns=delays, ring_window_offsets=dict(tx='0x80', rx='0x40'),
        arot_applied=False)
    report['firmware_path'] = '100aa40 -> combo100a918 (unsupported18) -> accel1006a14 -> 10091ec:9208 -> 9280; constant92b0=e00002c7'
    report['ios'] = 'b96aec sends exact OSData bytes; b96d70 ignores unsupported and continues; ba0c34 does not require orientation'
    report['key19'] = 'combo100a918 -> 1070ab8; version3 minimum16 and period-product bounds satisfied; no need to change payload'
    report['construction_snapshot_sha256'] = hashlib.sha256(frozen).hexdigest()
    report['device_verified_after_fix'] = False
    (OUT / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    path = OUT / 'capture-fixture.h'
    path.write_text(fixture())
    path.chmod(0o600)
    print(f'PASS captured 1a/0b success,18 unsupported; firmware unconditional rejection; iOS optional ARot; key19 version3/16 valid: {OUT}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--function', action='append', default=[], type=lambda x: int(x, 16))
    ap.add_argument('--window', action='append', default=[], help='hex address:size')
    ap.add_argument('--refresh', action='store_true', help='refresh shared iOS evidence')
    ap.add_argument('--firmware', action='store_true', help='export/name all established firmware handlers in the private Ghidra project')
    args = ap.parse_args()
    if args.firmware:
        args.function = list(dict.fromkeys([*FW_FUNCTIONS, *args.function]))
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == FW_SHA
    OUT.mkdir(parents=True, exist_ok=True)
    if args.window:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    for window in args.window:
        address, size = (int(x, 16) for x in window.split(':'))
        if address >= 0x106e000:
            source = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop_ext-20H350.bin').read_bytes()
            assert hashlib.sha256(source).hexdigest() == EXT_SHA
            origin = 0x106e000
        else:
            source, origin = fw, BASE
        data = source[address - origin:address - origin + size]
        assert len(data) == size
        decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        decoder.skipdata = True
        (OUT / f'{address:08x}-window.asm').write_text(''.join(
            f'{i.address:08x} {i.bytes.hex():12s} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
    if not args.function:
        if not args.window:
            evidence(args.refresh)
        return
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import TaskMonitor
    from ghidra.program.model.symbol import SourceType
    from java.io import ByteArrayInputStream
    from java.math import BigInteger
    project = pyghidra.open_project(str(ROOT / 'build/periph/aop-attach/firmware'), 'receiver')
    try:
        with pyghidra.program_context(project, '/pinned-aop.elf') as program:
            af = program.getAddressFactory()
            tx = program.startTransaction('Stage3 calibration entry points')
            try:
                ext_addr = af.getAddress('0106e000')
                if program.getMemory().getBlock(ext_addr) is None:
                    ext = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop_ext-20H350.bin').read_bytes()
                    assert hashlib.sha256(ext).hexdigest() == EXT_SHA
                    block = program.getMemory().createInitializedBlock(
                        'aop-ext', ext_addr, ByteArrayInputStream(ext), len(ext), TaskMonitor.DUMMY, False)
                    block.setExecute(True)
                    program.getProgramContext().setValue(program.getRegister('TMode'),
                        ext_addr, ext_addr.add(len(ext) - 1), BigInteger.ONE)
                for address in args.function:
                    a = af.getAddress(f'{address:x}')
                    DisassembleCommand(a, None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        assert CreateFunctionCmd(a).applyTo(program), hex(address)
                    func = program.getFunctionManager().getFunctionAt(a)
                    if address in FW_FUNCTIONS:
                        func.setName(FW_FUNCTIONS[address], SourceType.USER_DEFINED)
                # The assertion failure never returns. Without this annotation
                # Ghidra can merge unrelated following functions into panic tails.
                panic = af.getAddress('010087c8')
                if program.getFunctionManager().getFunctionAt(panic) is None:
                    DisassembleCommand(panic, None, True).applyTo(program)
                    assert CreateFunctionCmd(panic).applyTo(program)
                program.getFunctionManager().getFunctionAt(panic).setNoReturn(True)
                for address in args.function:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    CreateFunctionCmd.fixupFunctionBody(program, func, TaskMonitor.DUMMY)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in args.function:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    result = dec.decompileFunction(func, 60, TaskMonitor.DUMMY)
                    assert result.decompileCompleted(), str(result.getErrorMessage())
                    (OUT / f'{address:08x}.c').write_text(str(result.getDecompiledFunction().getC()))
                    (OUT / f'{address:08x}.asm').write_text('\n'.join(
                        str(i.getAddress()) + ' ' + str(i) for i in
                        program.getListing().getInstructions(func.getBody(), True)) + '\n')
            finally:
                dec.dispose()
            program.save('Stage3 calibration evidence', TaskMonitor.DUMMY)
    finally:
        project.close()
    print(f'PASS firmware exports: {OUT}')


if __name__ == '__main__':
    main()
