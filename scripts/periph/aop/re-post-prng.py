#!/usr/bin/env python3
"""Pinned post-ECID continuation; offline Capstone and optional own Ghidra project."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[3]
BASE = 0x01000000
PINS = ('af5d51a368ceb89dc5c359e7c84e54cb6082e308956be742c17622eb4a6047c8',
        '2528d2686ed1262de9910c2d60e45b68137e50aa601a0f8d54ea7d1347b9de22')
FUNCTIONS = {
    0x1004020: 'aop_prng_set_software_methods',
    0x100406a: 'aop_prng_seed_software',
    0x10042a4: 'aop_prng_mix_inputs',
    0x10043f0: 'aop_prng_device_info',
    0x102b698: 'aop_control_device_dispatch',
    0x102bae0: 'aop_control_device_enable',
    0x102bb98: 'aop_control_device_poll',
    0x1031002: 'aop_mmu_set_domain_access',
    0x10329a8: 'aop_power_task',
    0x1032c18: 'aop_power_transition',
    0x1032bde: 'aop_power_notify_pending',
    0x1032f18: 'aop_power_current_state',
    0x1034e9c: 'aop_prng_dispatch',
    0x107206c: 'aop_ext_platform_start',
    0x1073b40: 'aop_ext_create_service_tasks',
    0x1073b90: 'aop_ext_phase1_task',
    0x1033a8c: 'aop_rtkit_worker',
    0x1031054: 'aop_rtkit_send_hello',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ghidra', action='store_true')
    args = parser.parse_args()
    source = ROOT / 'build/periph/aop-r2/firmware/apple/aop'
    main_fw, ext = [(source / n).read_bytes() for n in
                    ('ipad6faop-20H350.bin', 'ipad6faop_ext-20H350.bin')]
    assert tuple(hashlib.sha256(d).hexdigest() for d in (main_fw, ext)) == PINS
    image = main_fw.ljust(0x6e000, b'\0') + ext
    out = ROOT / 'build/periph/aop-force-cold/re-post-prng'
    out.mkdir(parents=True, exist_ok=True)

    def word(address):
        return struct.unpack_from('<I', image, address - BASE)[0]

    # Raw table lookup, independent of the decompiler's switch recovery.
    table = 0x102b6ba
    method20 = table + 2 * struct.unpack_from('<H', image, table - BASE + 19 * 2)[0]
    assert word(0x1072310) == 0x1047598
    assert word(0x1047598) == 0x1039b34
    assert word(0x1039b34) == 0x102b699
    assert method20 == 0x102b736
    assert word(0x10475a0) == 0x10008000 and word(0x10475a4) == 2
    assert word(0x10475a8) == 4 and word(0x10475b4) == 4
    assert ext[0x4204:0x4206] == bytes.fromhex('1421')
    evidence = dict(main_sha256=PINS[0], ext_sha256=PINS[1],
                    control_object='01047598', control_name='CPU', dispatcher='0102b698',
                    post_prng_method=20, method20_target=f'{method20:08x}',
                    mmio_pa='0x210008000', register_offset=4, set_mask=4,
                    rmw_store='0102bb52', exact_register_function='unresolved',
                    fence_va='01072204', fence_ext_offset='0x4204',
                    fence_original='1421', fence_replacement='fee7',
                    fence_marker='[01052ab8+0xc] == 0x21',
                    observed_device_pc=False, cause_proven=False)
    (out / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    if args.ghidra:
        decompile(out, image)
        print(f'PASS {len(FUNCTIONS)} named functions exported from the private post-PRNG Ghidra project; {out}')
        return
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    windows = ((0x1034f98, 0x90), (0x1004020, 0xca), (0x10042a4, 0x14c),
               (0x10721e4, 0x38), (0x107221c, 0xc4),
               (0x102b736, 0x10), (0x102bae0, 0xb8), (0x102bb98, 0x78),
               (0x1031002, 0x50), (0x10329a8, 0x1d0), (0x1032c18, 0x2b8),
               (0x1073b40, 0x190))
    for address, size in windows:
        data = image[address - BASE:address - BASE + size]
        (out / f'{address:08x}.asm').write_text(''.join(
            f'{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}\n'
            for i in decoder.disasm(data, address)))
    print(f'PASS pinned software PRNG -> root 0x21 -> method20 -> MMIO 0x210008004 |= 4; {out}')


def decompile(out, image):
    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from java.math import BigInteger
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import TaskMonitor
    from ghidra.program.model.symbol import SourceType
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    eh = struct.pack('<16sHHIIIIIHHHHHH', ident, 2, 40, 1, 0x107206d,
                     52, 0, 0x05000000, 52, 32, 1, 0, 0, 0)
    ph = struct.pack('<IIIIIIII', 1, 0x1000, BASE, BASE, len(image), len(image), 7, 0x1000)
    elf = out / 'post-prng.elf'
    elf.write_bytes((eh + ph).ljust(0x1000, b'\0') + image)
    project = pyghidra.open_project(str(out), 'post-prng', create=True)
    try:
        program_path = '/' + elf.name
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
            tx = program.startTransaction('Post-PRNG boundaries')
            try:
                if fresh:
                    program.getProgramContext().setValue(program.getRegister('TMode'),
                        af.getAddress('010004e0'), af.getAddress('010389b7'), BigInteger.ONE)
                    program.getProgramContext().setValue(program.getRegister('TMode'),
                        af.getAddress('0106e000'), af.getAddress(f'{BASE + len(image) - 1:x}'), BigInteger.ONE)
                for address, name in FUNCTIONS.items():
                    a = af.getAddress(f'{address:x}')
                    DisassembleCommand(a, None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        if not CreateFunctionCmd(a).applyTo(program):
                            raise RuntimeError(f'cannot create {name}')
                    func = program.getFunctionManager().getFunctionAt(a)
                    func.setName(name, SourceType.USER_DEFINED)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in FUNCTIONS:
                    func = program.getFunctionManager().getFunctionAt(af.getAddress(f'{address:x}'))
                    result = dec.decompileFunction(func, 60, TaskMonitor.DUMMY)
                    if not result.decompileCompleted():
                        raise RuntimeError(str(result.getErrorMessage()))
                    (out / f'{address:08x}.c').write_text(str(result.getDecompiledFunction().getC()))
            finally:
                dec.dispose()
            program.save('Post-PRNG control flow', TaskMonitor.DUMMY)
    finally:
        project.close()


if __name__ == '__main__':
    main()
