#!/usr/bin/env python3
"""Stage charge9 into the reused charge tree, only on a9/charge9; no device I/O."""
from pathlib import Path
import shutil
import subprocess
ROOT = Path('/work')
TREE = Path('/src/wt/charge')
BASE = '824a8bda2cd69634884d8830224a41aa3ceecd93'
def main():
    git = ['git','-C',str(TREE)]
    assert subprocess.check_output(git+['branch','--show-current'],text=True).strip()=='a9/charge9'
    subprocess.run(git+['merge-base','--is-ancestor',BASE,'HEAD'],check=True)
    for f in ('apple_tristar_power.c','apple_j71t_charger.c'):
        shutil.copyfile(ROOT/'scripts/periph/kernel'/f,TREE/'drivers/power/supply'/f)
    shutil.copyfile(ROOT/'scripts/periph/kernel/sysfs-apple-charge9',TREE/'Documentation/ABI/testing/sysfs-apple-charge9')
    patch=ROOT/'scripts/periph/charge9-support.patch'
    if subprocess.run(git+['apply','--reverse','--check',str(patch)],capture_output=True).returncode:
        subprocess.run(git+['apply','--whitespace=error',str(patch)],check=True)
    print('PASS staged charge9 (branch guard, no device access)')
if __name__=='__main__':main()
