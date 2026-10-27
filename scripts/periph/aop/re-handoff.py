#!/usr/bin/env python3
"""Pinned sleep/wake (handoff) evidence for the AOP; host only, no device.

Default: export the shared iOS RTBuddy wake path (cached JSON unless
--refresh) and execute the pinned firmware's real Thumb power codec.
--firmware: decompile the firmware power task/transition/HELLO paths in the
private receiver project (run with the ghidra-mcp uv tool's python).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/periph/aop-handoff/re'
CAL = runpy.run_path(str(Path(__file__).with_name('re-calibration.py')))
IOS = {
    'ios-power-transition': 'fffffff005b65b54',
    'ios-send-wake220': 'fffffff005b71fc4',
    'ios-wait-boot-state': 'fffffff005b66630',
    'ios-wait-boot-gated': 'fffffff005b666e8',
    'ios-wait-boot-polled': 'fffffff005b66650',
    'ios-handle-hello': 'fffffff005b7196c',
    'ios-handle-epmap': 'fffffff005b71afc',
    'ios-mgmt-dispatch': 'fffffff005b71018',
    'ios-iop-power': 'fffffff005b71750',
    'ios-ap-ready': 'fffffff005b716d0',
    'ios-ap-power': 'fffffff005b71504',
}
FW_FUNCTIONS = {
    0x103131c: 'rtk_mgmt_rx',
    0x10314f4: 'rtk_power_decode',
    0x1031408: 'rtk_power_encode',
    0x1031444: 'rtk_iop_power_ack',
    0x1031498: 'rtk_ap_power_ack',
    0x1032ed0: 'rtk_iop_set_target',
    0x10329a8: 'rtk_power_task',
    0x1032bb6: 'rtk_power_callback_walk',
    0x1032c18: 'rtk_power_transition',
}


def codec(fw):
    """Real firmware decode/encode for every iOS table word and both ACKs."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
    from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    u.mem_map(0x1000000, 0x60000)
    u.mem_write(0x1000000, fw)
    u.mem_map(0x2000000, 0x2000)

    def call(address, value):
        u.reg_write(UC_ARM_REG_SP, 0x2001000)
        u.reg_write(UC_ARM_REG_LR, 0x2000001)
        u.reg_write(UC_ARM_REG_R0, value)
        u.emu_start(address | 1, 0x2000000, count=100)
        assert u.reg_read(UC_ARM_REG_PC) == 0x2000000
        return u.reg_read(UC_ARM_REG_R0)

    expected = {0x201: (8, 0x201), 0x220: (0x21, 0x20), 0x202: (8, 0x201), 0: (1, 0),
                0x10: (0x20, 0x10), 0x20: (0x21, 0x20)}
    records = []
    for wire, (state, ack) in expected.items():
        assert call(0x10314f4, wire) == state, hex(wire)
        assert call(0x1031408, state) == ack, hex(state)
        records.append(dict(request=hex(wire), internal=hex(state), ack=hex(ack)))
    return records


def ios_exports(refresh):
    result = {}
    for name, address in IOS.items():
        path = OUT / (name + '.json')
        if refresh or not path.exists():
            path.write_text(subprocess.check_output([sys.executable, '-B', str(ROOT / 'scripts/ghidra.py'),
                'decompile_function_by_address', f'address={address}'], text=True))
        data = json.loads(path.read_text())
        assert data['code'] and not data['truncated'], name
        (OUT / (name + '.c')).write_text(data['code'])
        result[name] = data['code']
    return result


def firmware(extra):
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import TaskMonitor
    from ghidra.program.model.symbol import SourceType
    targets = dict(FW_FUNCTIONS)
    targets.update({a: None for a in extra})
    project = pyghidra.open_project(str(ROOT / 'build/periph/aop-attach/firmware'), 'receiver')
    try:
        with pyghidra.program_context(project, '/pinned-aop.elf') as program:
            af = program.getAddressFactory()
            tx = program.startTransaction('AOP handoff power path')
            try:
                for address, name in targets.items():
                    a = af.getAddress(f'{address:x}')
                    DisassembleCommand(a, None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        assert CreateFunctionCmd(a).applyTo(program), hex(address)
                    func = program.getFunctionManager().getFunctionAt(a)
                    if name:
                        func.setName(name, SourceType.USER_DEFINED)
                    CreateFunctionCmd.fixupFunctionBody(program, func, TaskMonitor.DUMMY)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in targets:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    res = dec.decompileFunction(func, 60, TaskMonitor.DUMMY)
                    assert res.decompileCompleted(), str(res.getErrorMessage())
                    (OUT / f'fw-{address:08x}.c').write_text(str(res.getDecompiledFunction().getC()))
                    (OUT / f'fw-{address:08x}.asm').write_text('\n'.join(
                        str(i.getAddress()) + ' ' + str(i) for i in
                        program.getListing().getInstructions(func.getBody(), True)) + '\n')
            finally:
                dec.dispose()
            program.save('AOP handoff power path', TaskMonitor.DUMMY)
    finally:
        project.close()
    print(f'PASS firmware exports: {OUT}')


def tag_table_refs():
    """Every reference into the tag table, plus code that loads its address."""
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    project = pyghidra.open_project(str(ROOT / 'build/periph/aop-attach/firmware'), 'receiver')
    found = []
    try:
        with pyghidra.program_context(project, '/pinned-aop.elf') as program:
            af = program.getAddressFactory()
            refs = program.getReferenceManager()
            listing = program.getListing()
            functions = program.getFunctionManager()
            begin, end = 0x10389b8, 0x10389b8 + 0x1df
            for address in range(begin - 8, end):
                for ref in refs.getReferencesTo(af.getAddress(f'{address:x}')):
                    source = ref.getFromAddress()
                    func = functions.getFunctionContaining(source)
                    insn = listing.getInstructionAt(source)
                    found.append(dict(target=hex(address), source=str(source), type=str(ref.getReferenceType()),
                                      write=bool(ref.getReferenceType().isWrite()),
                                      function=func.getName() if func else None,
                                      instruction=str(insn) if insn else None))
    finally:
        project.close()
    (OUT / 'tag-table-refs.json').write_text(json.dumps(found, indent=2) + '\n')
    print(f'tag-table references: {len(found)}, writes: {sum(r["write"] for r in found)}')
    for r in found:
        print(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--refresh', action='store_true', help='re-query the shared iOS Ghidra server')
    ap.add_argument('--firmware', action='store_true', help='decompile firmware power paths (private project)')
    ap.add_argument('--function', type=lambda x: int(x, 16), action='append', default=[])
    ap.add_argument('--tag-refs', action='store_true', help='list firmware references into the tag table (private project)')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.tag_refs:
        tag_table_refs()
        return
    if args.firmware:
        firmware(args.function)
        return
    fw = (ROOT / 'build/periph/aop-stage3/root-firmware/ipad6faop-20H350.bin').read_bytes()
    assert hashlib.sha256(fw).hexdigest() == CAL['FW_SHA']
    ios = ios_exports(args.refresh)
    # The iOS resume path wakes after a provider RUN cycle; HELLO is accepted
    # only in status 4, while a bare type7/0x20 alone reaches boot state 8.
    assert '0x60000000000220' in ios['ios-send-wake220']
    assert '== 4' in ios['ios-handle-hello'] and 'RTBuddyManagement_sendWake220' in ios['ios-power-transition']
    assert 'iVar6 == 8' in ios['ios-wait-boot-gated']
    asm = OUT / 'fw-01032c18.asm'
    transition = None
    if asm.exists():
        text = asm.read_text()
        # Sleep target 8: ACK 201, WFI, then restore the previous state and
        # ACK it after the power-up callbacks. No HELLO sender in this path.
        for needle in ('01032d84 bl 0x01031444', '01032d98 bl 0x0102760c',
                       '01032e2e bl 0x01032ec0', '01032e64 bl.ne 0x01031444'):
            assert needle in text, needle
        transition = dict(sleep_ack='01032d84 rtk_iop_power_ack(8)=0x201', wfi='01032d98 aop_cpu_wfi',
                          restore='01032dea target==8 -> current=8, target=previous',
                          callbacks='01032e2e power-up clients', resume_ack='01032e64 rtk_iop_power_ack(previous)=0x20')
    # Tag table 0x010389b8 (size 0x1df): its only literal is at 0x1000b6c,
    # loaded by 0x1000b5c which READS STKG into the guard 0x01047474.
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    words = [(i.mnemonic, i.op_str) for i in Cs(CS_ARCH_ARM, CS_MODE_THUMB).disasm(fw[0xb5c:0xb6a], 0x1000b5c)]
    assert words == [('ldr', 'r0, [pc, #0xc]'), ('add.w', 'r1, r0, #8'), ('ldr', 'r2, [r0, #4]'),
                     ('ldr', 'r0, [pc, #8]'), ('b.w', '#0x1000b84')], words
    assert struct.unpack_from('<II', fw, 0xb6c) == (0x010389b8, 0x01047474)
    literals = [hex(0x1000000 + i) for i in range(0, len(fw) - 3, 4)
                if struct.unpack_from('<I', fw, i)[0] == 0x010389b8]
    assert literals == ['0x1000b6c'], literals
    report = dict(firmware_sha256=CAL['FW_SHA'], codec=codec(fw), hardware_success_verified=False,
                  sram_span='0x389b8: boot+text, tag table excluded',
                  tag_table=dict(address='0x010389b8', size='0x1df', literal_refs=literals,
                                 reader='0x1000b5c copies STKG to 0x01047474 (load only)',
                                 ghidra_direct_refs='none (tag-table-refs.json)'),
                  wake_word='0060000000000220',
                  ios_resume='b65b54 case1: provider start(fw, iopStatus==3), status 4, b71fc4 sends 220, '
                             'b66630 waits for status 8 (b71018: type7 low 0x20) or HELLO (b7196c, status 4 only)',
                  ios_sleep='b65b54 case0/2: request 201/202, wait, then provider stop (RUN cycle)',
                  firmware_transition=transition,
                  hello_after_wake_without_run_cycle='unproven: the driver classifies the first reply',
                  buffer_policy='b767f8: nonzero PA maps the existing buffer, no reply; PA 0 reuses it if large enough and replies')
    (OUT / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS pinned codec: 220 -> internal 21 -> ACK 20; 201 -> 8 -> 201; 10 -> 20 -> 10')
    print(f'PASS iOS exports: {len(ios)} functions under {OUT}')


if __name__ == '__main__':
    main()
