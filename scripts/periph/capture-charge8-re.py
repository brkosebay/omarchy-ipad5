#!/usr/bin/env python3
"""Repeat the offline Ghidra evidence capture used for charging round 8."""
from pathlib import Path
import subprocess
import json
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/charge8/re'
FUNCTIONS = ('005e34408', '005e4d9ac', '005e4d028', '005e23a90', '005e236c0',
             '005de8328', '005de35dc', '005de4024', '005de3f78', '005e50e0c', '005fee258', '005ff5ed0',
             '005ff5a08', '005febe00')
MEMORY = {'005fe52dc': 584, '0054cbd50': 28, '006e52918': 8,
          '006e53208': 8, '006e52980': 8}

def capture(tool, **args):
    text = subprocess.check_output(['python3', '-B', str(ROOT / 'scripts/ghidra.py'),
                                   tool, *(f'{k}={v}' for k,v in args.items())], text=True)
    return json.loads(text)

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    info = capture('get_program_info')
    assert info['sha256'] == 'c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    (OUT / 'program.json').write_text(json.dumps(info, indent=2)+'\n')
    for low in FUNCTIONS:
        address = 'fffffff' + low
        r = capture('decompile_function_by_address', address=address)
        (OUT / (address+'.json')).write_text(json.dumps(r, indent=2)+'\n')
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN)
    for low, length in MEMORY.items():
        address = 'fffffff' + low
        r = capture('read_memory', address=address, length=length)
        (OUT / (address+'-memory.json')).write_text(json.dumps(r, indent=2)+'\n')
        if low == '005fe52dc':
            (OUT / (address+'.asm')).write_text(''.join(
                f'{i.address:x} {i.mnemonic} {i.op_str}\n'
                for i in md.disasm(bytes.fromhex(r['hex']), int(address,16))))
    print('PASS charge8 kernelcache evidence captured (no device I/O)')
if __name__ == '__main__':
    main()
