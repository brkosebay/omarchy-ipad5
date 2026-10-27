#!/usr/bin/env python3
"""Apply Round 6 only to a9/periph, exporting exact offline test inputs."""
from pathlib import Path
import shutil

root = Path('/src/wt/periph')
if Path.cwd() != root:
    raise SystemExit('only /src/wt/periph is authorized')
shutil.copyfile('/work/scripts/periph/kernel/apple_j71t_charger.c',
                root / 'drivers/power/supply/apple_j71t_charger.c')


def replace(path, before, after):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise SystemExit(f'{path}: expected one source hunk')
    path.write_text(text.replace(before, after, 1))


replace(root / 'include/linux/power/bq27xxx_battery.h',
        '\tbool cache_only;\n',
        '\tbool cache_only;\n\t/* At least one complete cache-only safety sample succeeded. */\n\tbool cache_ready;\n')
p = root / 'drivers/power/supply/bq27xxx_battery.c'
replace(p, '\tif (di->cache_only)\n\t\tbq27xxx_battery_init_cache(&di->cache);\n',
        '\tif (di->cache_only) {\n\t\tbq27xxx_battery_init_cache(&di->cache);\n\t\tdi->cache_ready = false;\n\t}\n')
replace(p, '\tbq27xxx_cache_value(&cache.temperature, 0, &temperature);\n',
        '\tbq27xxx_cache_value(&cache.temperature, 0, &temperature);\n\t/* Optional measurements do not determine safety-sample readiness. */\n\tdi->cache_ready = true;\n')
replace(p, '\tdev_warn_ratelimited(di->dev, "battery cache refresh failed: %d\\n",\n\t\t\t     ret);',
        '\tif (di->cache_ready)\n\t\tdev_warn_ratelimited(di->dev, "battery cache refresh failed: %d\\n", ret);\n\telse\n\t\tdev_dbg(di->dev, "battery cache warming up: %d\\n", ret);')
replace(p, '\t\t\t\t   msecs_to_jiffies(di->cache_refresh_ms));\n',
        '\t\t\t\t   msecs_to_jiffies(di->cache_ready ?\n\t\t\t\t\t\t    di->cache_refresh_ms : 1000));\n')
needle = '\tmutex_lock(&di->lock);\n\tif (psp != POWER_SUPPLY_PROP_PRESENT && di->cache.flags < 0) {'
replace(p, needle, '\tmutex_lock(&di->lock);\n\t/* Presence is also unknown until the first complete safety sample. */\n\tif (!di->cache_ready) {\n\t\tret = -ENODATA;\n\t\tgoto out_unlock;\n\t}\n\tif (psp != POWER_SUPPLY_PROP_PRESENT && di->cache.flags < 0) {')
p = root / 'drivers/power/supply/power_supply_core.c'
replace(p, '\tret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val);\n\tif (ret)\n\t\treturn ret;',
        '\tret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val);\n\t/* The thermal core retries EAGAIN quietly instead of logging no data. */\n\tif (ret == -ENODATA)\n\t\treturn -EAGAIN;\n\tif (ret)\n\t\treturn ret;')
output = Path('/work/build/periph/round6-battery')
output.mkdir(parents=True, exist_ok=True)
for rel in ('drivers/power/supply/bq27xxx_battery.c',
            'include/linux/power/bq27xxx_battery.h',
            'drivers/power/supply/power_supply_sysfs.c',
            'drivers/power/supply/power_supply_core.c',
            'drivers/thermal/thermal_core.c'):
    (output / Path(rel).name).write_bytes((root / rel).read_bytes())
print('Applied Round 6 startup cache/retry and thermal no-data handling')
