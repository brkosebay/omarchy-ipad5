#!/usr/bin/env python3
"""Retain charge9's replays and add captured BC-before-enumeration inputs."""
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/charge10/tests'
DIR=ROOT/'docs/periph/logs/charge9-20261027'
def main():
    subprocess.run(['python3','-B','-I',str(ROOT/'scripts/periph/charge9-trace-fixture.py')],check=True)
    rows=list(csv.DictReader((DIR/'charge9-watch.tsv').open(),delimiter='\t'))
    failed=[r for r in rows if 'stage=usb-active' in r['detector'] and 'source_type=2 ' in r['reason']]
    assert failed and all(r['current_max_ua']=='1500000' and r['input_limit_ua']=='962500' for r in failed)
    dmesg=(DIR/'dmesg-and-state.txt').read_text()
    match=re.search(r'\[\s*([\d.]+)\].*charger detected type 2 budget 1500 mA dp (\d+) dm (\d+)',dmesg)
    when=float(match[1]);dp=int(match[2]);dm=int(match[3])
    ids=[float(x) for x in re.findall(r'\[\s*([\d.]+)\].*digital ID 10 09 08',dmesg)]
    id_time=next(x for x in ids if x>when)
    values=dict(CAPTURE_C10_DP_MV=dp,CAPTURE_C10_DM_MV=dm,
                CAPTURE_C10_ID_DELAY_MS=round((id_time-when)*1000),
                CAPTURE_C10_OLD_TYPE=2,CAPTURE_C10_OLD_LIMIT_UA=1500000)
    OUT.mkdir(parents=True,exist_ok=True)
    base=ROOT/'build/charge9/tests/capture.h'
    (OUT/'capture.h').write_text(base.read_text()+''.join(f'#define {k} {v}\n' for k,v in values.items()))
    (OUT/'capture.json').write_text(json.dumps(dict(values=values,rows=len(rows),
        failure_rows=len(failed),prior=json.loads((base.parent/'capture.json').read_text()),
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (DIR/'charge9-watch.tsv',DIR/'dmesg-and-state.txt')}),indent=2)+'\n')
    print(f'PASS charge10 capture: {len(rows)} rows, {len(failed)} active-USB rows stuck at 1.5A, BC -> ID after {values["CAPTURE_C10_ID_DELAY_MS"]}ms -> enumeration; round9 fixtures retained')
if __name__=='__main__':main()
