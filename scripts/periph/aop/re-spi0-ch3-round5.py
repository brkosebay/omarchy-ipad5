#!/usr/bin/env python3
"""Offline wake8 evidence: SCM flags, INTC event-3 reset, MPU restart.

Run: uv run --with capstone --with pyghidra python -B <this file>
No device connection. Reuses the private, SHA-pinned firmware project.
"""
import hashlib
import json
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[3]
OLD = runpy.run_path(str(Path(__file__).with_name('re-spi0-ch3-round4.py')))
OUT = ROOT / 'build/periph/aop-stage3/re/spi0-ch3-round5'
TARGETS = {
    0x0102c524: 'aop_interrupt_controller_command',
    0x0102c972: 'aop_interrupt_write_enabled',
    0x0102c9d2: 'aop_interrupt_restore_masks',
    0x0102f0c8: 'AOPSPIMaster_transferIRQ',
    0x0102eeb8: 'AOPSPIMaster_readSCMState',
    0x0102ef2e: 'AOPSPIMaster_stop',
    0x0102f2aa: 'AOPSPIMaster_start',
    0x0102f2f4: 'AOPSPIMaster_reset',
    0x0100e8b6: 'MPU6xxx_setStreamingEnabled',
    0x0100ede4: 'MPU6xxx_readRegister',
    0x0100edfe: 'MPU6xxx_writeRegister',
    0x0100ee18: 'MPU6xxx_startAsyncRead',
    0x0100eee4: 'MPU6xxx_setFIFOEnabled',
    0x0100ef14: 'MPU6xxx_setFIFODataPathEnabled',
}


def main():
    OLD['verify_inputs']()
    raw = OLD['FW_PATH'].read_bytes()
    log = ROOT / 'docs/periph/logs/aop-ch3tdw-20261027/reload-trigger-dmesg.txt'
    text = log.read_text()
    for word in ('scm_lo=00000003', 'c3_interval=01507725 c3_cursor=00000000',
                 'SPI0 CH3TD check errno=0', 'samples=31/31 elapsed_ms=184',
                 'stage3 T 0050002800000002 status=-110', 'AOP crashlog section='):
        assert word in text, word
    # Exact Thumb bytes, including BOTH ordering loads hidden by decompilation.
    assert raw[0x2c592:0x2c5a6].hex() == 'c0f80818a068d0f80818c0f80858a068d0f80808'
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    dis = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    OUT.mkdir(parents=True, exist_ok=True)
    # The older Ghidra function body omits switch arms; preserve linear bytes too.
    for start, end, label in ((0x2c58c, 0x2c5a8, 'intc-reset'),
                               (0x2c5a8, 0x2c65c, 'intc-sleep'),
                               (0x2c9d2, 0x2ca20, 'intc-restore'),
                               (0x2e110, 0x2f37c, 'spi-master')):
        (OUT / f'{label}.asm').write_text(''.join(
            f'{i.address:08x} {i.mnemonic} {i.op_str}\n'
            for i in dis.disasm(raw[start:end], 0x01000000 + start)))
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from ghidra.app.decompiler import DecompInterface
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.symbol import SourceType
    from ghidra.util.task import TaskMonitor
    project = pyghidra.open_project(str(OLD['PROJECT']), 'receiver')
    try:
        with pyghidra.program_context(project, OLD['PROGRAM']) as program:
            af = program.getAddressFactory()
            fm = program.getFunctionManager()
            tx = program.startTransaction('wake8 SCM and MPU restart evidence')
            try:
                for address, name in TARGETS.items():
                    at = af.getAddress(f'{address:x}')
                    fn = fm.getFunctionAt(at)
                    if fn is None:
                        DisassembleCommand(at, None, True).applyTo(program)
                        CreateFunctionCmd(at).applyTo(program)
                        fn = fm.getFunctionAt(at)
                    assert fn is not None, hex(address)
                    fn.setName(name, SourceType.USER_DEFINED)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address, name in TARGETS.items():
                    fn = fm.getFunctionAt(af.getAddress(f'{address:x}'))
                    result = dec.decompileFunction(fn, 120, TaskMonitor.DUMMY)
                    assert result.decompileCompleted(), result.getErrorMessage()
                    (OUT / f'{address:08x}-{name}.c').write_text(str(result.getDecompiledFunction().getC()))
            finally:
                dec.dispose()
            program.save('wake8 SCM and MPU restart evidence', TaskMonitor.DUMMY)
    finally:
        project.close()
    (OUT / 'inventory.json').write_text(json.dumps(dict(
        main_sha256=OLD['FW_SHA256'], ext_sha256=OLD['EXT_SHA256'],
        wake7_log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
        reset=[dict(pc='0102c592', offset='808', store=1),
               dict(pc='0102c598', offset='808', operation='read'),
               dict(pc='0102c59c', offset='808', store=0),
               dict(pc='0102c5a2', offset='808', operation='read')],
        scm=dict(value=3, flags=[0, 1], position=0, channel=3,
                 writable_proven=False, zero_after_reset_required=False),
        hardware_verified=False, functions={f'{a:08x}': n for a, n in TARGETS.items()}
    ), indent=2) + '\n')
    print(f'PASS pinned wake8 RE export: {OUT}')


if __name__ == '__main__':
    main()
