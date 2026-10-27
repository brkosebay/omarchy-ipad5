#!/usr/bin/env python3
"""Treat unavailable cached charge_now as no data, not a failed battery uevent.

Run through scripts/build-kernel.sh --run periph. Changes only the owned
kernel source and exports exact test inputs under build/periph; never commits
or accesses hardware.
"""
import argparse
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, default=Path('/src/wt/periph'))
    args = ap.parse_args()
    root = args.kernel.resolve()
    if root != Path('/src/wt/periph'):
        raise SystemExit('only /src/wt/periph is authorized')
    path = root / 'drivers/power/supply/bq27xxx_battery.c'
    before = '''	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27xxx_cached_value(&di->cache.charge_now, val);
		break;
'''
    after = '''	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27xxx_cached_value(&di->cache.charge_now, val);
		/* An unavailable optional HDQ sample must not abort the uevent. */
		if (ret)
			ret = -ENODATA;
		break;
'''
    source = path.read_text()
    if after not in source:
        if source.count(before) != 1:
            raise SystemExit('expected exactly one cached charge_now case')
        path.write_text(source.replace(before, after, 1))
    output = Path('/work/build/periph/round5-battery')
    output.mkdir(parents=True, exist_ok=True)
    for relative in ('drivers/power/supply/bq27xxx_battery.c',
                     'drivers/power/supply/power_supply_sysfs.c',
                     'include/linux/power/bq27xxx_battery.h'):
        (output / Path(relative).name).write_bytes((root / relative).read_bytes())
    print('Applied cached charge_now no-data handling; exported actual C test inputs')


if __name__ == '__main__':
    main()
