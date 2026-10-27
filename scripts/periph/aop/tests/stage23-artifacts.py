#!/usr/bin/env python3
"""Exercise the real warm merger and AFK reservation checks using built DTBs."""
import copy
import importlib.machinery
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
CHECK = importlib.machinery.SourceFileLoader(
    'stage23_memory_test', str(ROOT / 'scripts/periph/aop/check-stage23-memory.py')).load_module()
FDT = CHECK.FDT


def main():
    old_path = ROOT / 'build/kernel-tri48a/dtbs/s8003-j71t.dtb'
    new_path = ROOT / 'build/kernel-tri49a/dtbs/s8003-j71t.dtb'
    old, _, _ = FDT.parse(old_path.read_bytes())
    new_blob = new_path.read_bytes()
    CHECK.check(new_blob)
    with tempfile.TemporaryDirectory(prefix='aop-stage23-fdt-') as work:
        work = Path(work)
        running = copy.deepcopy(old)
        nodes = FDT.index(running)
        nodes['/memory@800000000'].props['reg'] = struct.pack('>QQ', 0x800000000, 0x7bbe4000)
        # A real console region must survive BOTH experiment and recovery FDTs.
        log = FDT.Node('ramoops@87b800000')
        log.props.update({'compatible': b'ramoops\0', 'no-map': b'',
                          'reg': struct.pack('>QQ', 0x87b800000, 0x200000),
                          'console-size': struct.pack('>I', 0x100000),
                          'record-size': struct.pack('>I', 0x100000)})
        nodes['/reserved-memory'].children[log.name] = log
        # Representative bootloader header reservations, well away from the pool.
        running_path = work / 'running.dtb'
        running_path.write_bytes(FDT.serialize(running, [(0x860000000, 0x100000),
                                                       (0x87b800000, 0x200000)], 0))
        merged_path = work / 'merged.dtb'
        command = ['python3', '-B', str(ROOT / 'scripts/boot/device/linux-a9-fdt-merge')]
        subprocess.run(command + ['--old', str(old_path), '--running', str(running_path),
                                  '--new', str(new_path), '-o', str(merged_path)], check=True)
        good = merged_path.read_bytes()
        assert FDT.index(FDT.parse(good)[0])['/reserved-memory/ramoops@87b800000'].props == log.props
        iomem = ('800000000-857ffffff : System RAM\n'
                 '858000000-8583fffff : a9-aop-stage3-pool\n'
                 '858400000-87bbe3fff : System RAM\n')
        CHECK.check(good, iomem, runtime=True)
        # Booting a pre-experiment recovery DT must retain the header reserve.
        recovery = work / 'recovery.dtb'
        subprocess.run(command + ['--old', str(new_path), '--running', str(merged_path),
                                  '--new', str(old_path), '-o', str(recovery)], check=True)
        _, reserved, _ = FDT.parse(recovery.read_bytes())
        assert (0x87b800000, 0x200000) in reserved
        assert FDT.index(FDT.parse(recovery.read_bytes())[0])['/reserved-memory/ramoops@87b800000'].props == log.props
        assert (CHECK.POOL, CHECK.SIZE) in reserved
        if b'aop-firmware@210e00000' in new_blob:
            assert (0x210e00000, 0x9b000) in reserved
        # Check negative cases using the exact production checker.
        for bad_iomem in ('800000000-87bbe3fff : System RAM\n',
                          '000000000-000000000 : System RAM\n'):
            try:
                CHECK.check(good, bad_iomem, runtime=True)
            except AssertionError:
                pass
            else:
                raise AssertionError('unsafe/hidden iomem accepted')
        tree, reserved, cpuid = FDT.parse(good)
        del FDT.index(tree)[CHECK.POOL_PATH].props['no-map']
        try:
            CHECK.check(FDT.serialize(tree, reserved, cpuid), runtime=True)
        except AssertionError:
            pass
        else:
            raise AssertionError('missing no-map accepted')
        tree, reserved, cpuid = FDT.parse(good)
        try:
            CHECK.check(FDT.serialize(tree, [r for r in reserved if r != (CHECK.POOL, CHECK.SIZE)], cpuid))
        except AssertionError:
            pass
        else:
            raise AssertionError('missing persistent reservation accepted')
    print('PASS stage23 real warm merger: new pool/phandle/RAM fixups, recovery retention, unsafe iomem/no-map/header refusal')


if __name__ == '__main__':
    main()
