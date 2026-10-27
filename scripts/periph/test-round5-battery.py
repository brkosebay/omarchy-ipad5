#!/usr/bin/env python3
"""Compile actual cached-battery and uevent functions with host-only mocks."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess


def extract(source, name):
    pattern = r'^static (?:inline )?(?:int|void)\s+' + re.escape(name) + r'\s*\([\s\S]*?^}'
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError(f'actual function missing: {name}')
    return match.group()


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct mutex { int held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = 0; }
union power_supply_propval { int intval; const char *strval; };
struct power_supply { void *battery_info; };
enum { POWER_SUPPLY_TECHNOLOGY_UNKNOWN, POWER_SUPPLY_TECHNOLOGY_LION };
#define BQ27XXX_O_MUL_CHEM 1
#define BQ27XXX_MANUFACTURER "Texas Instruments"
struct device { int dummy; };
struct device_attribute { enum power_supply_property property; };
struct power_supply_attr { struct device_attribute dev_attr; const char *prop_name; };
struct kobj_uevent_env { int emitted; unsigned properties; };
static int power_supply_battery_info_get_prop(void *info, enum power_supply_property prop,
                                             union power_supply_propval *value)
{ (void)info; (void)prop; (void)value; return -EINVAL; }
static int bq27xxx_battery_capacity_level(struct bq27xxx_device_info *di,
                                         union power_supply_propval *value)
{ (void)di; value->intval = 3; return 0; }
static int bq27xxx_battery_read_health(struct bq27xxx_device_info *di,
                                      union power_supply_propval *value)
{ (void)di; value->intval = 1; return 0; }
static int power_supply_format_property(struct device *dev, bool uevent,
                                        struct device_attribute *attr, char *buf);
static int add_uevent_var(struct kobj_uevent_env *env, const char *fmt, ...)
{
    (void)fmt;
    ++env->emitted;
    return 0;
}
'''


TESTS = r'''
static struct bq27xxx_device_info di;
static struct power_supply psy;
static int power_supply_format_property(struct device *dev, bool uevent,
                                        struct device_attribute *attr, char *buf)
{
    union power_supply_propval value = { .intval = 0x12345678 };
    (void)dev; (void)uevent;
    int ret = bq27xxx_battery_get_cached_property(&psy, &di, attr->property, &value);
    if (ret) return ret;
    return snprintf(buf, 64, "%d\n", value.intval);
}
static int get(enum power_supply_property prop, int *out)
{
    union power_supply_propval value = { .intval = *out };
    int ret = bq27xxx_battery_get_cached_property(&psy, &di, prop, &value);
    *out = value.intval;
    assert(!di.lock.held);
    return ret;
}
int main(void)
{
    union power_supply_propval sample;
    int out = 0x12345678;
    bq27xxx_battery_init_cache(&di.cache);
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == -ENODATA);
    assert(out == 0x12345678);
    di.cache.flags = 0; /* Other core measurements succeeded. */
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == -ENODATA);

    /* Successful values, including a real empty-battery zero, are retained. */
    sample.intval = 3200000;
    bq27xxx_cache_value(&di.cache.charge_now, 0, &sample);
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == 0 && out == 3200000);
    sample.intval = 0;
    bq27xxx_cache_value(&di.cache.charge_now, 0, &sample);
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == 0 && out == 0);

    const int errors[] = {-ENODATA, -EIO, -ETIMEDOUT, -ENXIO, -EAGAIN, -ENODEV};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        sample.intval = 3200000;
        bq27xxx_cache_value(&di.cache.charge_now, 0, &sample);
        sample.intval = 9999999; /* A failed read must never become a sample. */
        bq27xxx_cache_value(&di.cache.charge_now, errors[i], &sample);
        assert(di.cache.charge_now.error == errors[i]);
        assert(di.cache.charge_now.value == 3200000);
        out = 0x12345678;
        assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == -ENODATA);
        assert(out == 0x12345678); /* Neither fabricated zero nor stale value. */
    }
    sample.intval = 3500000;
    bq27xxx_cache_value(&di.cache.charge_now, 0, &sample);
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == 0 && out == 3500000);

    /* Gauge-wide and other properties' genuine failures remain genuine errors. */
    di.cache.flags = -EIO;
    assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &out) == -EIO);
    assert(get(POWER_SUPPLY_PROP_PRESENT, &out) == 0 && out == 0);
    di.cache.flags = 0;
    di.cache.voltage.error = -EIO;
    assert(get(POWER_SUPPLY_PROP_VOLTAGE_NOW, &out) == -EIO);
    di.cache.charge_full.error = -ETIMEDOUT;
    assert(get(POWER_SUPPLY_PROP_CHARGE_FULL, &out) == -ETIMEDOUT);

    /* Actual power_supply core omits unavailable charge_now and continues. */
    struct device dev = {0};
    struct kobj_uevent_env env = {0};
    char buf[64];
    sample.intval = 2;
    bq27xxx_cache_value(&di.cache.status, 0, &sample);
    bq27xxx_cache_value(&di.cache.charge_now, -EIO, &sample);
    sample.intval = 8036000;
    bq27xxx_cache_value(&di.cache.charge_full, 0, &sample);
    const enum power_supply_property properties[] = {
        POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CHARGE_NOW, POWER_SUPPLY_PROP_CHARGE_FULL
    };
    for (unsigned i = 0; i < sizeof(properties) / sizeof(properties[0]); ++i)
        assert(add_prop_uevent(&dev, &env, properties[i], buf) == 0);
    assert(env.emitted == 2);
    sample.intval = 3500000;
    bq27xxx_cache_value(&di.cache.charge_now, 0, &sample);
    assert(add_prop_uevent(&dev, &env, POWER_SUPPLY_PROP_CHARGE_NOW, buf) == 0);
    assert(env.emitted == 3 && strcmp(buf, "3500000") == 0);
    bq27xxx_cache_value(&di.cache.charge_full, -EIO, &sample);
    assert(add_prop_uevent(&dev, &env, POWER_SUPPLY_PROP_CHARGE_FULL, buf) == -EIO);
    puts("PASS: actual battery cache/getter and power_supply uevent functions; startup, optional errors, recovery, valid zero and mandatory-error preservation");
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[2]
    output = root / 'build/periph/round5-battery'
    source_path = output / 'bq27xxx_battery.c'
    source = source_path.read_text()
    header = (output / 'bq27xxx_battery.h').read_text()
    sysfs = (output / 'power_supply_sysfs.c').read_text()
    names = ('bq27xxx_cache_value', 'bq27xxx_battery_init_cache',
             'bq27xxx_simple_value', 'bq27xxx_cached_value',
             'bq27xxx_battery_get_cached_property')
    functions = '\n\n'.join(extract(source, name) for name in names)
    functions += '\n\n' + extract(sysfs, 'add_prop_uevent')
    properties = sorted(set(re.findall(r'POWER_SUPPLY_PROP_[A-Z_]+', functions)))
    enums = 'enum power_supply_property {' + ','.join(properties) + '};\n'
    structs = '\n'.join(re.search(r'struct ' + name + r'\s*\{.*?\n\};', header, re.S).group()
                        for name in ('bq27xxx_cached_value', 'bq27xxx_reg_cache'))
    structs += '\nstruct bq27xxx_device_info { struct mutex lock; struct bq27xxx_reg_cache cache; unsigned opts; };\n'
    # Insert real cache structures after the mutex type and before mocks use them.
    split = 'struct power_supply_attr {'
    start, tail = PRELUDE.split(split, 1)
    prelude = start + structs + split + tail
    attrs = 'static struct power_supply_attr power_supply_attrs[] = {\n' + '\n'.join(
        f'  [{name}] = {{{{{name}}}, "{name.removeprefix("POWER_SUPPLY_PROP_")}"}},' for name in properties) + '\n};\n'
    c_file, executable = output / 'actual-battery-test.c', output / 'actual-battery-test'
    c_file.write_text(enums + prelude + attrs + functions + TESTS)
    cc = shutil.which('clang') or shutil.which('cc')
    if not cc:
        raise SystemExit('host C compiler required')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(c_file), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
    (output / 'tested-source-sha256.txt').write_text(digest + '\n')
    print('Battery source SHA256:', digest)


if __name__ == '__main__':
    main()
