#!/usr/bin/env python3
"""Focused, host-only Thumb analysis of the pinned AOP management receiver.

Run using the ghidra-mcp Python environment. Uses its own ignored project;
never opens or replaces the shared kernelcache project. Firmware addresses
are linked virtual addresses, not AP physical addresses.
"""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[3]
BASE = 0x01000000
FUNCTIONS = (0x10311ac, 0x103131c, 0x1031538, 0x1033cf8,
             0x102b2a0, 0x102b03c, 0x1033c34,
             0x100f864, 0x100f5dc, 0x100f27c,
             0x1031054, 0x1031408, 0x10314f4, 0x1032ed0, 0x1032ee8)


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cold-stall", action="store_true")
    parser.add_argument("--cold-wait", action="store_true", help="decode cache-off PRNG fatal, tasks and RTKit state")
    parser.add_argument("--windows-only", action="store_true", help="export Capstone cold windows without opening Ghidra")
    args = parser.parse_args()
    args.cold_stall |= args.cold_wait
    if args.windows_only and not args.cold_stall:
        parser.error("--windows-only requires --cold-stall")
    functions = FUNCTIONS
    if args.cold_stall:
        functions += (0x102c524, 0x102c66c, 0x10341f0, 0x1031be6,
                      0x102b3bc, 0x102ce1c, 0x102cf94, 0x1030340,
                      0x1029a98, 0x1029b94, 0x10298b4, 0x1034e9c,
                      0x1033a8c, 0x103403c, 0x10293c4)
    if args.cold_wait:
        functions += (0x1034a44, 0x1034a2c, 0x10328d8, 0x102760c,
                      0x103310c, 0x1033324, 0x1033354, 0x1032f18,
                      0x1029654, 0x102969c, 0x1029c9c, 0x10297a8,
                      0x1033a38, 0x1000b74)
    raw = (ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop-20H350.bin').read_bytes()
    if hashlib.sha256(raw).hexdigest() != 'af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8':
        raise SystemExit('Refusing another firmware image')
    out = ROOT / 'build/periph/aop-attach/firmware'
    out.mkdir(parents=True, exist_ok=True)
    # Minimal ELF32 wrapper, unchanged bytes in one segment at the linked VA.
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    eh = struct.pack('<16sHHIIIIIHHHHHH', ident, 2, 40, 1, FUNCTIONS[0] | 1,
                     52, 0, 0x05000000, 52, 32, 1, 0, 0, 0)
    ph = struct.pack('<IIIIIIII', 1, 0x1000, BASE, BASE, len(raw), 0x6e000, 7, 0x1000)
    elf = out / 'pinned-aop.elf'
    elf.write_bytes((eh + ph).ljust(0x1000, b'\0') + raw)
    tables = {}
    for offset, count, first in ((0x311e4, 8, 1), (0x31336, 16, 0), (0xf8b6, 6, 0xa0)):
        tables[hex(BASE + offset)] = {
            hex(first + i): hex(BASE + offset + raw[offset + i] * 2)
            for i in range(count)}
    (out / 'jump-tables.json').write_text(json.dumps(tables, indent=2) + '\n')
    # Keep full windows when computed jumps/panic tails confuse function bodies.
    windows = {f'{BASE + offset:08x}': raw[offset:offset + size].hex()
               for offset, size in ((0x311ac, 0x170), (0x3131c, 0xec),
                                    (0x31538, 0x8c), (0xf5dc, 0x288),
                                    (0xf864, 0x178), (0xf27c, 0x148),
                                    (0x34744, 0x30))}
    (out / 'instruction-windows.json').write_text(json.dumps(windows, indent=2) + '\n')

    if args.windows_only:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
        ext = (ROOT / 'build/periph/aop-r2/firmware/apple/aop/ipad6faop_ext-20H350.bin').read_bytes()
        assert hashlib.sha256(ext).hexdigest() == '2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22'
        for name, data, address, size, mode in (
            ('reset-cache-clear', raw, BASE+0x98, 0x20, CS_MODE_ARM),
            ('reset-cache-enable', raw, BASE+0x298, 0x18, CS_MODE_ARM),
            ('c-start-and-ctors', raw, BASE+0x27390, 0x94, CS_MODE_THUMB),
            ('guard-import', raw, BASE+0xb5c, 0x10, CS_MODE_THUMB),
            ('intc-pending-loop', raw, BASE+0x2c638, 0x26, CS_MODE_THUMB),
            ('platform-init-calls', raw, BASE+0x3445e, 0x88, CS_MODE_THUMB),
            ('ext-platform-call', ext, BASE+0x721d0, 0x40, CS_MODE_THUMB),
            ('prng-dispatch', raw, BASE+0x34e9c, 0x226, CS_MODE_THUMB),
            ('prng-init-and-ecid-check', raw, BASE+0x34ece, 0x148, CS_MODE_THUMB),
            ('fatal-print-wait', raw, BASE+0x34a2c, 0x44, CS_MODE_THUMB),
            ('fatal-idle', raw, BASE+0x328d8, 0x10, CS_MODE_THUMB),
            ('wfi', raw, BASE+0x2760c, 0x38, CS_MODE_THUMB),
            ('fatal-record', raw, BASE+0x3310c, 0x210, CS_MODE_THUMB),
            ('task-create', raw, BASE+0x293c4, 0x26c, CS_MODE_THUMB),
            ('task-ready-enqueue', raw, BASE+0x29654, 0x3c, CS_MODE_THUMB),
            ('task-ready-dequeue', raw, BASE+0x29a54, 0x44, CS_MODE_THUMB),
            ('scheduler', raw, BASE+0x2969c, 0xf8, CS_MODE_THUMB),
            ('ext-main', ext, BASE+0x7206c, 0x1b0, CS_MODE_THUMB),
        ):
            origin = BASE+0x6e000 if data is ext else BASE
            offset = address-origin
            decoder = Cs(CS_ARCH_ARM, mode)
            (out / f'cold-{name}.asm').write_text(''.join(
                f'{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}\n'
                for i in decoder.disasm(data[offset:offset+size], address)))

        return

    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from java.math import BigInteger
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import TaskMonitor
    from ghidra.program.model.symbol import SourceType
    project = pyghidra.open_project(str(out), 'receiver', create=True)
    program_path = '/' + elf.name
    try:
        fresh = project.getProjectData().getFile(program_path) is None
        if fresh:
            loaded = pyghidra.program_loader().project(project).source(str(elf)).language(
                'ARM:LE:32:v8').compiler('default').load()
            try:
                program_path = str(loaded.getPrimary().save(TaskMonitor.DUMMY).getPathname())
            finally:
                loaded.close()
        with pyghidra.program_context(project, program_path) as program:
            af = program.getAddressFactory()
            tx = program.startTransaction('Pinned receiver Thumb entry points')
            try:
                context = program.getProgramContext()
                if fresh:
                    context.setValue(program.getRegister('TMode'), af.getAddress('010004e0'),
                                     af.getAddress('010389b7'), BigInteger.ONE)
                for address in functions:
                    a = af.getAddress(f'{address:x}')
                    DisassembleCommand(a, None, True).applyTo(program)
                    first = program.getListing().getInstructionAt(a)
                    if first is not None:
                        DisassembleCommand(first.getMaxAddress().add(1), None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        cmd = CreateFunctionCmd(a)
                        if not cmd.applyTo(program):
                            raise RuntimeError(str(cmd.getStatusMsg()))
                    func = program.getFunctionManager().getFunctionAt(a)
                    if args.cold_wait and address in (0x1034a44, 0x1034a2c, 0x10328d8, 0x1000b74):
                        func.setNoReturn(True)
                    CreateFunctionCmd.fixupFunctionBody(program, func, TaskMonitor.DUMMY)
                    names = {0x10341f0: 'aop_platform_device_dispatch',
                             0x102c524: 'aop_interrupt_controller_dispatch',
                             0x102c66c: 'aop_interrupt_dispatch',
                             0x102ce1c: 'aop_mailbox_configure',
                             0x102b3bc: 'aop_map_device_if_needed',
                             0x1029a98: 'aop_mutex_lock',
                             0x1029b94: 'aop_mutex_trylock',
                             0x10298b4: 'aop_mutex_unlock',
                             0x10293c4: 'aop_task_create',
                             0x1034e9c: 'aop_prng_dispatch',
                             0x1034a44: 'aop_panic_printf',
                             0x1034a2c: 'aop_fatal_stop',
                             0x10328d8: 'aop_fatal_idle_forever',
                             0x102760c: 'aop_cpu_wfi',
                             0x103310c: 'aop_record_fatal',
                             0x1033324: 'aop_fatal_callback_once',
                             0x1029654: 'aop_task_enqueue',
                             0x102969c: 'aop_schedule',
                             0x10297a8: 'aop_task_diagnostic_table'}
                    if args.cold_stall and address in names:
                        func.setName(names[address], SourceType.USER_DEFINED)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in functions:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    result = dec.decompileFunction(func, 60, TaskMonitor.DUMMY)
                    if not result.decompileCompleted():
                        raise RuntimeError(str(result.getErrorMessage()))
                    (out / f'{address:08x}.c').write_text(str(result.getDecompiledFunction().getC()))
                    lines = [str(i.getAddress()) + ' ' + str(i) for i in
                             program.getListing().getInstructions(func.getBody(), True)]
                    (out / f'{address:08x}.asm').write_text('\n'.join(lines) + '\n')
            finally:
                dec.dispose()
            program.save('Pinned firmware receiver', TaskMonitor.DUMMY)
    finally:
        project.close()
    print(f'Pinned receiver analysis: {out}')


if __name__ == '__main__':
    main()
