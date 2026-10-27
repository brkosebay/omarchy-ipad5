#!/usr/bin/env python3
"""Install power-owned round-5 sources and the all-load 1848 MHz OPP mapping."""
from pathlib import Path
import re
import shutil
import subprocess

if (Path.cwd() != Path('/src/wt/power') or
        subprocess.check_output(['git', 'branch', '--show-current'], text=True).strip() != 'a9/power'):
    raise SystemExit('Run only with scripts/build-kernel.sh --run power ...')
for src, dst in (
        ('apple-a9-tuning.h', 'drivers/cpufreq/apple-a9-tuning.h'),
        ('apple-s8003-thermal.c', 'drivers/thermal/apple-s8003-thermal.c'),
        ('s8003-power.h', 'include/linux/soc/apple/s8003-power.h')):
    shutil.copyfile(Path('/work/scripts/power/kernel') / src, dst)
path = Path('arch/arm64/boot/dts/apple/s8003.dtsi')
source = path.read_text()
pattern = r'(\bopp08\s*\{[^}]*?opp-level\s*=\s*<)(8|15)(>;[^}]*\})'
source, count = re.subn(pattern, lambda m: m[1] + '15' + m[3], source)
if count != 1:
    raise SystemExit('unexpected 1848 MHz OPP; refusing to guess its mapping')
path.write_text(source)
