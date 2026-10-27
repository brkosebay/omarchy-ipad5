#!/usr/bin/env python3
"""Focused Ghidra fallback for missing function definitions in extracted kexts.

Run with the existing ghidra-mcp Python environment. All projects and outputs
are confined to build/power; this never opens or changes the shared project.
Examples: --binary build/power/round3-kexts/AppleEmbeddedTempSensor.macho
          --output build/power/pmu-local --function 0xfffffff005f1b714
Use --instructions ADDRESS:LENGTH for an arbitrary bounded instruction listing.
"""
import argparse
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--function', action='append', default=[])
    ap.add_argument('--instructions', action='append', default=[])
    ap.add_argument('--raw-base', type=lambda x: int(x, 0))
    args = ap.parse_args()
    binary = args.binary.resolve()
    binary.relative_to(root / 'build/power')
    out = args.output.resolve()
    out.relative_to(root / 'build/power')
    out.mkdir(parents=True, exist_ok=True)

    import pyghidra
    pyghidra.start(install_dir='/opt/homebrew/opt/ghidra/libexec')
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.model.address import AddressSet
    from ghidra.util.task import TaskMonitor
    project = pyghidra.open_project(str(out), 'focused', create=True)
    program_path = '/' + binary.name
    try:
        if project.getProjectData().getFile(program_path) is None:
            loader = pyghidra.program_loader().project(project).source(str(binary))
            if args.raw_base is not None:
                loader = (loader.loaders('BinaryLoader').language('AARCH64:LE:64:v8A')
                          .compiler('default'))
            loaded = loader.load()
            try:
                program_path = str(loaded.getPrimary().save(TaskMonitor.DUMMY).getPathname())
            finally:
                loaded.close()
        with pyghidra.program_context(project, program_path) as program:
            af = program.getAddressFactory()
            tx = program.startTransaction('Focused analyst entry points')
            try:
                if args.raw_base is not None:
                    program.setImageBase(af.getDefaultAddressSpace().getAddress(args.raw_base), True)
                for address in args.function:
                    a = af.getAddress(address.removeprefix('0x'))
                    DisassembleCommand(a, None, True).applyTo(program)
                    if program.getFunctionManager().getFunctionAt(a) is None:
                        cmd = CreateFunctionCmd(a)
                        if not cmd.applyTo(program):
                            raise RuntimeError(str(cmd.getStatusMsg()))
                for spec in args.instructions:
                    address, length = spec.split(':')
                    a = af.getAddress(address.removeprefix('0x'))
                    end = a.add(int(length, 0) - 1)
                    for offset in range(0, int(length, 0), 4):
                        entry = a.add(offset)
                        if program.getListing().getInstructionAt(entry) is None:
                            DisassembleCommand(entry, AddressSet(a, end), False).applyTo(program)
            finally:
                program.endTransaction(tx, True)
            dec = DecompInterface()
            dec.openProgram(program)
            try:
                for address in args.function:
                    a = af.getAddress(address.removeprefix('0x'))
                    function = program.getFunctionManager().getFunctionAt(a)
                    result = dec.decompileFunction(function, 90, TaskMonitor.DUMMY)
                    record = {'address': str(a), 'function': str(function),
                              'error': str(result.getErrorMessage()),
                              'code': str(result.getDecompiledFunction().getC())
                              if result.decompileCompleted() else None}
                    (out / (str(a) + '.json')).write_text(json.dumps(record, indent=2) + '\n')
                for spec in args.instructions:
                    address, length = spec.split(':')
                    a = af.getAddress(address.removeprefix('0x'))
                    end = a.add(int(length, 0) - 1)
                    lines = [str(i.getAddress()) + ' ' + str(i)
                             for i in program.getListing().getInstructions(AddressSet(a, end), True)]
                    (out / (str(a) + '.asm')).write_text('\n'.join(lines) + '\n')
            finally:
                dec.dispose()
            program.save('Focused offline analysis', TaskMonitor.DUMMY)
    finally:
        project.close()


if __name__ == '__main__':
    main()
