#!/usr/bin/env python3
"""Linux-only fake-sysfs check that both diagnostic lines stay in one TSV row."""
import csv
import io
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    root=Path('/work')
    with tempfile.TemporaryDirectory(prefix='charge10-watch-') as d:
        base=Path(d)
        props={'usb':{'online':'1','usb_type':'Unknown [C]'},
               'charger':{'input_current_limit':'475000','charge7_state':'attach=2 active=1'},
               'battery':{p:'0' for p in ('current_now','voltage_now','temp','status')}}
        props['usb']['current_max']='3000000'
        for supply, files in props.items():
            (base/supply).mkdir()
            for name,value in files.items():(base/supply/name).write_text(value+'\n')
        state=base/'lightning_state'
        state.write_text('charger_detect: stage=complete\nsource_epoch: usb_idle_events=15\n')
        env=dict(os.environ,WATCH_SECONDS='1',CHARGE10_BOUNDED='1',
                 SYSFS_POWER_SUPPLY=str(base),TRISTAR_STATE=str(state))
        out=subprocess.check_output(['sh',str(root/'scripts/periph/charge10-watch.sh')],env=env,text=True,timeout=5)
        rows=list(csv.DictReader(io.StringIO(out),delimiter='\t'))
        assert rows and all(len(r)==12 and None not in r for r in rows)
        assert all('stage=complete' in r['detector'] and 'usb_idle_events=15' in r['detector'] for r in rows)
        print('PASS charge10 logger: both diagnostic lines in one bounded fake-sysfs TSV row, 12 columns')
if __name__=='__main__':main()
