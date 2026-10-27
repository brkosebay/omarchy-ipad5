#!/usr/bin/env python3
"""Install the Round 5 J71t charger template in its isolated kernel tree."""
from pathlib import Path
import shutil

if Path.cwd() != Path('/src/wt/periph'):
    raise SystemExit('only /src/wt/periph is authorized')
shutil.copyfile('/work/scripts/periph/kernel/apple_j71t_charger.c',
                'drivers/power/supply/apple_j71t_charger.c')
