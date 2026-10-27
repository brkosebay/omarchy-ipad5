#!/usr/bin/env python3
"""Stage only charging round 8, reusing /src/wt/charge. Never accesses hardware."""
from pathlib import Path
import shutil
import subprocess

TREE = Path('/src/wt/charge')
ROOT = Path('/work')
BASE = 'a8aa6fea363394f3d21f2eef7525998884b4cf3f'

def main():
    def git(*args):
        return subprocess.check_output(['git', '-C', str(TREE), *args], text=True).strip()
    assert git('branch', '--show-current') == 'a9/charge8'
    subprocess.run(['git', '-C', str(TREE), 'merge-base', '--is-ancestor', BASE, 'HEAD'], check=True)
    for name in ('apple_j71t_charger.c', 'apple_tristar_power.c'):
        shutil.copyfile(ROOT / 'scripts/periph/kernel' / name, TREE / 'drivers/power/supply' / name)
    shutil.copyfile(ROOT / 'scripts/periph/kernel/sysfs-apple-charge8',
                    TREE / 'Documentation/ABI/testing/sysfs-apple-charge8')
    print('PASS staged charge8 on a9/charge8')

if __name__ == '__main__':
    main()
