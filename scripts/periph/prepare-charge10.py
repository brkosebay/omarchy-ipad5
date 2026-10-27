#!/usr/bin/env python3
"""Stage charge10 only on its kernel branch, with round9 support inherited."""
from pathlib import Path
import shutil
import subprocess
ROOT=Path('/work')
TREE=Path('/src/wt/charge')
BASE='16a34c8432c61937366964d493b82f387e7d51cc'
def main():
    git=['git','-C',str(TREE)]
    assert subprocess.check_output(git+['branch','--show-current'],text=True).strip()=='a9/charge10'
    subprocess.run(git+['merge-base','--is-ancestor',BASE,'HEAD'],check=True)
    shutil.copyfile(ROOT/'scripts/periph/kernel/apple_tristar_power.c',TREE/'drivers/power/supply/apple_tristar_power.c')
    shutil.copyfile(ROOT/'scripts/periph/kernel/sysfs-apple-charge10',TREE/'Documentation/ABI/testing/sysfs-apple-charge10')
    # Round9 PMIC, gauge and notifier code is inherited byte for byte.
    for name in ('drivers/power/supply/apple_j71t_charger.c',
                 'drivers/power/supply/bq27xxx_battery.c',
                 'drivers/usb/gadget/udc/core.c','include/linux/usb/gadget.h'):
        assert (TREE/name).read_bytes()==subprocess.check_output(git+['show',f'{BASE}:{name}'])
    print('PASS charge10 staged; inherited round9 charger/gauge/notifier code unchanged')
if __name__=='__main__':main()
