#!/usr/bin/env python3
"""Retain pinned iOS/firmware instruction evidence; host only, no device IO."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_ARCH_ARM, CS_MODE_THUMB
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/audio'))
import kcmap

def main():
    out = ROOT / 'build/periph/aop-force-cold/re'
    out.mkdir(parents=True, exist_ok=True)
    raw, kernel, _ = kcmap.load()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    (out / 'kernelcache.sha256').write_text(digest + '\n')
    decoder = Cs(CS_ARCH_ARM64, 0)
    for name, address, size in (
        ('stop', 0xfffffff005b4b8fc, 0x15c),
        ('start', 0xfffffff005b4b738, 0x1c4),
        ('run-autoboot', 0xfffffff005b4e7c8, 0x68),
        ('v2-class-identity', 0xfffffff005b4e378, 0x78),
        ('v2-read-register', 0xfffffff005b4e9b4, 0xc),
        ('vendor-running-test', 0xfffffff005b4e4b8, 0x24),
        ('v2-format-register-status', 0xfffffff005b4e624, 0x108),
        ('unmap-hook-ret', 0xfffffff005b4e7c4, 4),
        ('idle-stub', 0xfffffff005b4c460, 0x14),
        ('fifo-masks-outbox', 0xfffffff005b4e498, 0x18c),
        ('remap', 0xfffffff005b4e72c, 0x98),
        ('rtbuddy-power-transition', 0xfffffff005b65b54, 0x600),
        ('prepare-gates-mapper', 0xfffffff005b4b630, 0xa8),
        ('doorbell-not-reset', 0xfffffff005b4e830, 0x20),
        ('wake220', 0xfffffff005b71fc4, 0x58),
    ):
        data = kcmap.checked(raw, kcmap.va_offset(kernel, address, size), size)
        (out / f'{name}.asm').write_text('\n'.join(
            f'{i.address:016x} {i.mnemonic} {i.op_str}' for i in decoder.disasm(data, address)) + '\n')
    address = 0xfffffff006d3ff98
    data = kcmap.checked(raw, kcmap.va_offset(kernel, address, 0x6e0), 0x6e0)
    (out / 'vtable.txt').write_text(''.join(f'{off:03x} {struct.unpack_from("<Q", data, off)[0]:016x}\n'
                                         for off in (0x550, 0x580, 0x630, 0x638, 0x640, 0x678, 0x680, 0x6a8, 0x6b0, 0x6d8)))
    # Preserve the negative result: no independent CPU halt bit was found.
    capture = ROOT / 'docs/periph/logs/aop-cold2-20261025/registers.txt'
    pairs = re.findall(r'0b88=([0-9a-f]+).*?4008=([0-9a-f]+)', capture.read_text())
    assert pairs and all(a == b for a, b in pairs)
    (out / 'halt-evidence.json').write_text(json.dumps(dict(
        kernelcache_sha256=digest, v2_vtable='fffffff006d3ff98',
        read_register_slot='0x640', read_register='fffffff005b4e9b4',
        vendor_running_slot='0x680', vendor_running='fffffff005b4e4b8',
        vendor_test='read32(wrapper+0xb88) & 1',
        independent_cpu_halt_status_verified=False,
        cold2_capture=str(capture.relative_to(ROOT)),
        matching_b88_inbox_snapshots=len(pairs),
        implemented_predicate='RUN(+0x44 bit4)=0, auto-boot=0, outbox-enable=0, no mailbox faults after fixed 10ms delay',
        execution_proof=False, dma_quiescence_proof=False), indent=2) + '\n')
    fw = (ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for name, offset, size in (('fifo-receive', 0x2cdd8, 0x3e),
                                ('fifo-layout', 0x2ce1c, 0x160),
                                ('fifo-drain', 0x2cb0e, 0x48),
                                ('mailbox-bases', 0x342de, 0x60),
                                ('ext-tag-import', 0x28478, 0xc0),
                                ('ext-page-map', 0x30a2a, 0x40),
                                ('hello-send', 0x31054, 0x150),
                                ('guard-init', 0xb5c, 0x18)):
        (out / f'firmware-{name}.asm').write_text('\n'.join(
            f'{i.address:08x} {i.mnemonic} {i.op_str}'
            for i in decoder.disasm(fw[offset:offset + size], 0x1000000 + offset)) + '\n')
    tags = {}
    table, length = struct.unpack_from('<II', fw, 0x48)
    cursor = table
    while cursor < table + length:
        tag, size = struct.unpack_from('<II', fw, cursor)
        tags[tag.to_bytes(4, 'big').decode('ascii')] = dict(
            offset=hex(cursor + 8), size=size, value_le=fw[cursor+8:cursor+8+size].hex())
        cursor += 8 + size
    assert tags['RTSZ']['value_le'] == '00e00600'
    assert 'EC0v' not in tags and 'EC0s' not in tags
    # AOPb2 capture changes after release, but contains no SRAM capture or PC.
    capture = ROOT / 'docs/periph/logs/aop-cold2b-20261025'
    rows = []
    for line in (capture / 'registers.txt').read_text().splitlines():
        if 'event=control-after' in line:
            rows.append(dict(re.findall(r'(0044|0c00|4008|4020|4040|auto_boot)=([0-9a-f]+)', line)))
    (out / 'cold-hello-evidence.json').write_text(json.dumps(dict(
        kernelcache_sha256=digest, main_sha256=hashlib.sha256(fw).hexdigest(), tags=tags,
        ext_map_object_offset='0x47510',
        ext_map_object_le32=[hex(x) for x in struct.unpack_from('<8I', fw, 0x47510)],
        ios_remap=dict(pa_lo='0x10e00000', pa_hi=2, lower=0, upper='0x9b000', enable=1),
        ios_reset_pulse_found=False, ios_reset_vector_store_found=False,
        bounded_reset_search=['AppleA7IOP stop/start/prepare', 'V2 +6a8/+6b0/+6b8',
                              'empty j71t power/clock gate lists'],
        ios_wake_condition='b65cd0 compares saved old status to 3; b65cdc calls b71fc4',
        linux_old_status_verified=False, wake220_fix_hardware_verified=False,
        cold2b_last_control_snapshots=rows[-3:],
        cold2b_sram_or_pc_capture_available=False), indent=2) + '\n')
    print(out)
if __name__ == '__main__':
    main()
