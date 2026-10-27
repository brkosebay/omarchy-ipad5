#!/usr/bin/env python3
"""Fill missing touch decompilation in a private auxiliary Ghidra project.

Run with ~/.local/share/uv/tools/ghidra-mcp/bin/python, GHIDRA_INSTALL_DIR
set as for scripts/ghidra-mcp.sh. Never attaches to the shared server.
Only defines entries proven by direct BL instructions or kext vtables.
Usage: ghidra-offline.py HEX_ADDRESS [...]
"""
import json
from pathlib import Path
import struct
import sys
import pyghidra

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/touch/re'
SOURCE = ROOT / 'build/periph/kexts'


def main():
    info = json.loads((SOURCE / 'manifest.json').read_text())['kexts']['com.apple.driver.AppleMultitouchSPI']
    sections = info['sections']
    text = next(s for s in sections if s['name'] == '__text')
    low, high = text['va'], text['va'] + text['size']
    entries = set()
    for section in sections:
        if section['name'] in ('__const', '__mod_init_func') and 'path' in section:
            data = (SOURCE / section['path']).read_bytes()
            for pos in range(0, len(data) - 7, 8):
                value = struct.unpack_from('<Q', data, pos)[0]
                if low <= value < high and not value & 3:
                    entries.add(value)
    data = (SOURCE / text['path']).read_bytes()
    for pos in range(0, len(data) - 3, 4):
        instruction = struct.unpack_from('<I', data, pos)[0]
        if instruction & 0xfc000000 == 0x94000000:
            delta = instruction & 0x3ffffff
            if delta & 0x2000000:
                delta -= 0x4000000
            target = low + pos + delta * 4
            if low <= target < high:
                entries.add(target)
    targets = [int(a, 16) for a in sys.argv[1:]]
    OUT.mkdir(parents=True, exist_ok=True)
    with pyghidra.open_program(SOURCE / info['path'], project_location=OUT / 'ghidra-aux',
                              project_name='touch', analyze=False) as flat:
        from ghidra.app.decompiler import DecompInterface
        from ghidra.util.task import TaskMonitor
        program = flat.getCurrentProgram()
        manager = program.getFunctionManager()
        tx = program.startTransaction('Define touch direct calls and vtable entries')
        try:
            for address in sorted(entries):
                addr = flat.toAddr(f'{address:x}')
                if manager.getFunctionAt(addr) is None:
                    flat.disassemble(addr)
                    flat.createFunction(addr, f'mtspi_{address:x}')
        finally:
            program.endTransaction(tx, True)
        decompiler = DecompInterface()
        decompiler.openProgram(program)
        for address in targets:
            function = manager.getFunctionAt(flat.toAddr(f'{address:x}'))
            if function is None:
                print(f'No proven entry: {address:x}', flush=True)
                continue
            result = decompiler.decompileFunction(function, 45, TaskMonitor.DUMMY)
            output = {'address': f'{address:x}', 'name': str(function.getName()),
                      'code': str(result.getDecompiledFunction().getC()) if result.getDecompiledFunction() else None,
                      'error': str(result.getErrorMessage())}
            (OUT / f'{address:x}.json').write_text(json.dumps(output, indent=2) + '\n')
            instructions = program.getListing().getInstructions(function.getBody(), True)
            lines = []
            while instructions.hasNext():
                instruction = instructions.next()
                lines.append(str(instruction.getAddress()) + ' ' + str(instruction))
            (OUT / f'{address:x}.asm').write_text('\n'.join(lines) + '\n')
            print(f'{address:x}: {len(output["code"] or "")} chars', flush=True)
        decompiler.dispose()


if __name__ == '__main__':
    main()
