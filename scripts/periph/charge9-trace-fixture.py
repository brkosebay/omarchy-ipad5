#!/usr/bin/env python3
"""Distil charge8's captured failures into numeric inputs for round9 C replays."""
import csv
import hashlib
import json
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[2]
DIR=ROOT/'docs/periph/logs/charge8-20261027'
OUT=ROOT/'build/charge9/tests'
def pairs(s):return dict(re.findall(r'(\w+)=([^ ]+)',s))
def main():
    rows=list(csv.DictReader((DIR/'charge8-watch.tsv').open(),delimiter='\t'))
    dmesg=(DIR/'dmesg-and-state.txt').read_text()
    mac=[r for r in rows if r['online']=='1' and pairs(r['reason'])['source_type']=='1' and r['input_limit_ua']=='2400000']
    fast=[r for r in rows if pairs(r['reason'])['attach']=='0' and r['online']=='1' and r['input_limit_ua']=='100000']
    offline=[r for r in rows if r['online']=='0' and r['battery_status']=='Full' and int(r['battery_current_ua'])<0]
    bc=[r for r in rows if pairs(r['reason'])['ceiling_ua']=='1500000']
    cc=sorted(set(int(pairs(r['detector'])['cc_mv']) for r in rows if int(pairs(r['detector'])['cc_mv'])>1200))
    assert mac and fast and offline and bc and cc
    a=float(re.search(r'\[\s*([\d.]+)\].*source budget 500 mA: input 0x20,.*type 0',dmesg)[1])
    b=float(re.search(r'\[\s*([\d.]+)\].*PMU input reduced to 0x02.*',dmesg[dmesg.index('[  275.'):])[1])
    reset_ms=round((b-a)*1000)
    assert reset_ms==26
    values=dict(CAPTURE_ATTACH_RESET_MS=reset_ms,CAPTURE_CC_LOW_MV=min(cc),
                CAPTURE_CC_HIGH_MV=1625,CAPTURE_UNPLUG_UA=int(offline[0]['battery_current_ua']),
                CAPTURE_WALL_UA=int(rows[-1]['battery_current_ua']),
                CAPTURE_WALL_BC_TYPE=int(pairs(bc[0]['reason'])['source_type']))
    assert values['CAPTURE_WALL_BC_TYPE']==3
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'capture.h').write_text(''.join(f'#define {k} {v}\n' for k,v in values.items()))
    (OUT/'capture.json').write_text(json.dumps(dict(values=values,rows=len(rows),
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (DIR/'charge8-watch.tsv',DIR/'dmesg-and-state.txt')}),indent=2)+'\n')
    print(f'PASS captured fixture: {len(rows)} rows; Mac, fast swap, held unplug/Full-negative, clean wall replug; reset {reset_ms}ms and CC {cc}')
if __name__=='__main__':main()
