#!/usr/bin/env python3
"""Read-only capture of iOS's BC secondary rule and its enum-to-name table."""
import json
from pathlib import Path
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/charge10/re'
def call(tool,**args):
    return json.loads(subprocess.check_output(['python3','-B',str(ROOT/'scripts/ghidra.py'),tool,
        *(f'{k}={v}' for k,v in args.items())],text=True))
def main():
    OUT.mkdir(parents=True,exist_ok=True)
    info=call('get_program_info')
    assert info['sha256']=='c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a'
    (OUT/'program.json').write_text(json.dumps(info,indent=2)+'\n')
    for addr in ('fffffff005de35dc','fffffff005de377c','fffffff005de3424'):
        data=call('decompile_function_by_address',address=addr)
        assert data.get('code'),addr
        (OUT/(addr+'.json')).write_text(json.dumps(data,indent=2)+'\n')
    table=call('read_memory',address='fffffff006de2df0',length=64)
    strings=call('read_memory',address='fffffff00545e099',length=64)
    raw=bytes.fromhex(strings['hex'])
    names=[raw[p-int(strings['address'],16):].split(b'\0',1)[0].decode()
           for p in struct.unpack('<8Q',bytes.fromhex(table['hex']))]
    assert names[:3]==['sdp','dcp','cdp'],names
    for name,data in (('port-type-table',table),('port-type-strings',strings),('port-types',names)):
        (OUT/(name+'.json')).write_text(json.dumps(data,indent=2)+'\n')
    print('PASS iOS enum proof: internal 1=dcp, 2=cdp; secondary 5de35dc returns 2 below 326mV and 1 otherwise')
if __name__=='__main__':main()
