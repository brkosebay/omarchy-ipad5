#!/usr/bin/env python3
"""Reproduce the AOP reload-crash and sleep/wake RE; host only, no device.

The default mode checks the captured RTKit crashlog against the pinned firmware
and emits a compact evidence file.  ``--firmware`` also names/comments the
established SPI paths in the private receiver Ghidra project and exports them.
Run that mode with the ghidra-mcp virtualenv Python and GHIDRA_INSTALL_DIR set.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[3]
BASE = 0x01000000
FW_SHA = 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8'
FW_PATH = ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin'
CRASH_PATH = ROOT / 'docs/periph/logs/aop-handoff-20261026/quiesce-reload/crashlog.bin'
SEP_LOG_PATH = ROOT / 'docs/sep/logs/20261026-1400-linux-xart-reply-unconsumed.log'
SEP_DRIVER_PATH = ROOT / 'drivers/sep/sep-a9.c'
IOSERVICE_PATH = ROOT / 'build/firmware/private/IOService.txt'
OUT = ROOT / 'build/periph/aop-stage3/re/reload-wake'

FW_FUNCTIONS = {
    0x0102e660: 'AOPSPIMaster_handleEvent',
    0x0102f0c8: 'AOPSPIMaster_transferIRQ',
    0x0102f1b4: 'AOPSPIMaster_channelIRQ',
    0x0102f20a: 'AOPSPIMaster_channelFaultIRQ',
    0x0102f2aa: 'AOPSPIMaster_start',
    0x0102f2f4: 'AOPSPIMaster_reset',
}


def fourcc(raw):
    return ''.join(chr(b) if 32 <= b < 127 else '.' for b in struct.pack('>I', raw))


def sections(data):
    assert struct.unpack_from('<I', data)[0] == 0x434c4845  # CLHE
    used = struct.unpack_from('<I', data, 8)[0]
    assert used <= len(data)
    result = {}
    offset = 0x20
    while offset + 16 <= used:
        tag, _, _, size = struct.unpack_from('<4I', data, offset)
        if size < 16:
            break
        result[fourcc(tag)] = data[offset + 16:offset + size]
        offset += size
    return result


def verify():
    fw = FW_PATH.read_bytes()
    assert hashlib.sha256(fw).hexdigest() == FW_SHA
    crash = CRASH_PATH.read_bytes()
    sec = sections(crash)
    assert {'Cver', 'Ctim', 'CrgA', 'Ccst', 'Cmbx', 'Crtk', 'Ccdp', 'Csta'} <= sec.keys()
    assert bytes.fromhex('cce47592d067374e8b46151be730c1de') == sec['Cver'][:16]

    words = struct.unpack_from('<92I', sec['CrgA'])
    assert words[:19] == (
        4, 0x160,              # record version/stack-byte count
        0xc1100280,            # r0: SPI0 channel-3 register bank
        0x010427f8,            # r1: fresh per-channel pointer table
        0x00200029,            # r2
        0xb0bed0ff,            # r3
        0x01047e78,            # r4: static spi0 controller object
        0,                     # r5: NULL channel object
        0x280,                 # r6: channel-3 register stride
        0x01046f98,            # r7
        0x40,                  # r8: SPI0 +0x8d0 pending bit 2*3
        0,                     # r9
        6,                     # r10: 2*channel
        3,                     # r11: channel
        0x010272ed,            # r12
        0x01046f80,            # sp
        0x0102c6fb,            # lr: interrupt dispatcher return
        0x0102f252,            # pc
        0x800000b2,            # cpsr: Thumb IRQ mode
    )
    # RTKit's ARM register record appends fault status/address after its stack
    # snapshot.  LPAE DFSR 0x206 is a level-2 read translation fault; DFAR is
    # exactly the NULL object field read below.
    assert words[86:90] == (0x206, 0x4c, 0xffffffff, 0xffffffff)

    # Thumb-2 encodings, independently rendered in the saved Ghidra export:
    # BIC r2,#0x450; ORR r2,#0x20; LDR r5,[r1,r10,LSL#1]; LDRB r0,[r5,#0x4c].
    assert fw[0x2f23c:0x2f240] == bytes.fromhex('22f48a62')
    assert fw[0x2f248:0x2f24c] == bytes.fromhex('42f02002')
    assert fw[0x2f24e:0x2f252] == bytes.fromhex('51f81a50')
    assert fw[0x2f252:0x2f256] == bytes.fromhex('95f84c00')

    # Static spi0 descriptor/object: IOP address 0x10100000, space 2,
    # 0x1000 bytes, name pointer -> "spi0".  Runtime mapping in the crash is
    # c1100000; arm-io translates the physical peripheral to AP 0x210100000.
    assert struct.unpack_from('<4I', fw, 0x47e80) == (0x10100000, 2, 0x1000, 0)
    assert struct.unpack_from('<I', fw, 0x47e94)[0] == 0x01039759
    assert fw[0x39759:0x3975e] == b'spi0\0'

    # SEP supplies an independent A9-wrapper observation.  The five Linux
    # writes filled its AP->SEP FIFO and stayed there for 40 seconds.  SEP is
    # not merely similar hardware: the saved iOS registry binds role=SEP to
    # the same AppleA7IOPV2 class and iop,s8000 personality used by AOP.  Our
    # passive SEP driver only clears mailbox mask 0x1000 in the +0x4004 view;
    # it never replays the provider's bit0/+0xc04/+0x4020 activation sequence.
    sep_log = SEP_LOG_PATH.read_text()
    assert 'snd_sts=0xc101 (AP->SEP FIFO not empty, unchanged) tx=5 rx=0' in sep_log
    assert '## +40 s:' in sep_log
    sep_driver = SEP_DRIVER_PATH.read_text()
    assert 'writel(SEP_MBOX_INT_ENABLE, sep->mbox + SEP_MBOX_EN_INT);' in sep_driver
    assert '#define SEP_MBOX_INT_ENABLE\t0x1000' in sep_driver
    ioservice = IOSERVICE_PATH.read_text(errors='replace')
    sep_provider = ioservice.index('"role" = "SEP"')
    window = ioservice[max(0, sep_provider - 800):sep_provider + 800]
    assert 'AppleA7IOPV2' in window and '"IONameMatch" = "iop,s8000"' in window

    report = {
        'firmware_sha256': FW_SHA,
        'crashlog_sha256': hashlib.sha256(crash).hexdigest(),
        'fault': {
            'pc': '0x0102f252',
            'instruction': 'ldrb.w r0, [r5, #0x4c]',
            'dfsr': '0x206 (LPAE level-2 read translation fault)',
            'dfar': '0x4c',
            'context': 'CPSR 0x800000b2 (Thumb IRQ mode)',
        },
        'spi': {
            'handler': '0x0102f20a AOPSPIMaster_channelFaultIRQ',
            'controller_object': '0x01047e78',
            'runtime_base': '0xc1100000',
            'ap_physical_base': '0x210100000',
            'pending': 'base+0x8d0 = 0x40 -> channel 3',
            'channel_table': '0x010427f8; slot 3 = NULL',
            'channel_registers': 'base+0x280; clear 0x450 at +0, set 0x20 at +0xc',
            'reset': '0x0102f2f4: control &= ~0x101; assert 0x100, wait bit8/bit0, 100us, deassert, 100us, provider state3, restore',
        },
        'wake': {
            'ios_resume': 'b65b54 -> provider vtable+548(b4b738,resume=1) -> status4 -> b71fc4(220) -> b66630(wait state8)',
            'provider_resume': 'b4b738: setActive; mask 0x1111; unmask bit0 and bit12 in both views; enable AIC sources; set +0x4020 bit0; skip b4e7c8 RUN when resume=1',
            'nmi_not_used': 'b4e830 writes wrapper +0xc004=0x11 and +0xc024=1; no call on normal resume path',
            'sep_corroboration': {
                'device': 'five AP->SEP words left snd_sts=0xc101 nonempty for 40 seconds; tx=5 rx=0',
                'shared_provider': 'saved iOS registry: SEP role -> AppleA7IOPV2, IONameMatch=iop,s8000',
                'linux_gap': 'sep-a9 only writes +0x4004=0x1000; it omits bit0, mirrored +0xc04, +0x4020 bit0, and setActive',
                'ep0_op10': 'iOS requests AppleSEPManager PM state1 and sends no reply; it is not a mailbox acknowledgement',
            },
        },
        'device_verified_fix': False,
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS reload fault: SPI0 channel3 pending -> NULL +0x4c read; evidence: {OUT}')


def annotate_firmware():
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.model.listing import CodeUnit
    from ghidra.program.model.symbol import SourceType
    from ghidra.util.task import TaskMonitor

    project = pyghidra.open_project(str(ROOT / 'build/periph/aop-attach/firmware'), 'receiver')
    try:
        with pyghidra.program_context(project, '/pinned-aop.elf') as program:
            af = program.getAddressFactory()
            listing = program.getListing()
            tx = program.startTransaction('AOP reload crash SPI0 root cause')
            try:
                for address, name in FW_FUNCTIONS.items():
                    a = af.getAddress(f'{address:x}')
                    DisassembleCommand(a, None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        assert CreateFunctionCmd(a).applyTo(program), hex(address)
                    func = program.getFunctionManager().getFunctionAt(a)
                    func.setName(name, SourceType.USER_DEFINED)
                    CreateFunctionCmd.fixupFunctionBody(program, func, TaskMonitor.DUMMY)
                comments = {
                    0x0102f20a: 'SPI0 channel-fault IRQ: +0x8d0 bit (2*channel), then per-channel object table.',
                    0x0102f24e: 'Reload crash: status 0x40 selects channel 3, but fresh table slot 3 is NULL.',
                    0x0102f252: 'Captured PC. LDRB NULL+0x4c; CrgA DFSR=0x206, DFAR=0x4c, r5=0.',
                    0x0102f2aa: 'SPI0 start: provider state5, clock value, hardware reset, then control bit0.',
                    0x0102f2f4: 'Controller reset: clear ctl bits0/8, assert bit8, wait bit8=1/bit0=0, delays, provider state3, restore.',
                    0x01047e78: 'Static spi0 controller object; runtime hardware base at object+8 was 0xc1100000 in crash.',
                    0x010427f8: 'Crash-time freshly allocated channel-pointer table; slot3 was NULL while retained IRQ bit6 fired.',
                }
                for address, comment in comments.items():
                    listing.setComment(af.getAddress(f'{address:x}'), CodeUnit.PRE_COMMENT, comment)
            finally:
                program.endTransaction(tx, True)

            OUT.mkdir(parents=True, exist_ok=True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in FW_FUNCTIONS:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    result = dec.decompileFunction(func, 60, TaskMonitor.DUMMY)
                    assert result.decompileCompleted(), str(result.getErrorMessage())
                    (OUT / f'fw-{address:08x}.c').write_text(str(result.getDecompiledFunction().getC()))
                    (OUT / f'fw-{address:08x}.asm').write_text('\n'.join(
                        str(i.getAddress()) + ' ' + str(i)
                        for i in listing.getInstructions(func.getBody(), True)) + '\n')
            finally:
                dec.dispose()
            program.save('AOP reload crash SPI0 root cause', TaskMonitor.DUMMY)
    finally:
        project.close()
    print(f'PASS private firmware Ghidra annotations saved: {OUT}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--firmware', action='store_true', help='annotate/export private firmware Ghidra project')
    args = ap.parse_args()
    verify()
    if args.firmware:
        annotate_firmware()


if __name__ == '__main__':
    main()
