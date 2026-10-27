#!/usr/bin/env python3
"""Fault actual cache-refresh, uevent and thermal functions through startup."""
import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/periph/round6-battery'
spec = importlib.util.spec_from_file_location('prior_battery_test', Path(__file__).with_name('test-round5-battery.py'))
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)

EXTRA = r'''
struct thermal_zone_device {
    struct device device;
    struct power_supply *psy;
    unsigned long recheck_delay_jiffies;
    bool broken;
};
static unsigned long jiffies;
static unsigned cache_delay, thermal_delay;
static int refresh_stage, optional_error, refresh_warnings, refresh_debug;
static int cache_notifications, thermal_warnings;
static void *system_freezable_wq;
#define BQ27XXX_O_ZERO 2
#define BQ27XXX_REG_FLAGS 0
#define BQ27XXX_REG_TTE 1
#define WARN_ON(c) assert(!(c))
#define THERMAL_RECHECK_DELAY 250UL
#define THERMAL_MAX_RECHECK_DELAY 1000UL
#define max(a, b) ((a) > (b) ? (a) : (b))
#define dev_dbg(...) (++refresh_debug)
#define dev_warn_ratelimited(...) (++refresh_warnings)
#define dev_info(...) (++thermal_warnings)
static unsigned msecs_to_jiffies(unsigned value) { return value; }
static void queue_delayed_work(void *q, struct delayed_work *w, unsigned delay)
{ (void)q; (void)w; cache_delay = delay; }
static void power_supply_changed(struct power_supply *p) { (void)p; ++cache_notifications; }
static int bq27xxx_read(struct bq27xxx_device_info *d, int reg, bool single)
{ (void)d; (void)single; assert(reg == BQ27XXX_REG_FLAGS); return refresh_stage == 1 ? -EIO : 0; }
static int bq27xxx_battery_read_soc(struct bq27xxx_device_info *d)
{ (void)d; return refresh_stage == 2 ? -EIO : 42; }
static int bq27xxx_battery_current_and_status(struct bq27xxx_device_info *d,
        union power_supply_propval *current, union power_supply_propval *status,
        struct bq27xxx_reg_cache *cache)
{
    (void)d; (void)cache;
    if (refresh_stage == 3) return -EIO;
    current->intval = -671000; status->intval = 3; return 0;
}
static int bq27xxx_battery_voltage(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; if (refresh_stage == 4) return -EIO; v->intval = 3772000; return 0; }
static int bq27xxx_battery_read_temperature(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; if (refresh_stage == 5) return -EIO; v->intval = 286; return 0; }
static bool bq27xxx_cache_has_prop(struct bq27xxx_device_info *d, enum power_supply_property p)
{ (void)d; (void)p; return true; }
static int bq27xxx_battery_read_time(struct bq27xxx_device_info *d, int reg, union power_supply_propval *v)
{ (void)d; (void)reg; v->intval = 18000; return 0; }
static int bq27xxx_battery_read_charge_now(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; if (optional_error) return optional_error; v->intval = 3655000; return 0; }
static int bq27xxx_battery_read_fcc(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; v->intval = 8096000; return 0; }
static int bq27xxx_battery_read_cyct(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; v->intval = 237; return 0; }
static int bq27xxx_battery_pwr_avg(struct bq27xxx_device_info *d, union power_supply_propval *v)
{ (void)d; v->intval = -2530000; return 0; }
static struct power_supply *thermal_zone_device_priv(struct thermal_zone_device *tz)
{ return tz->psy; }
static void thermal_zone_device_set_polling(struct thermal_zone_device *tz, unsigned long delay)
{ (void)tz; thermal_delay = delay; }
static void thermal_zone_broken_disable(struct thermal_zone_device *tz) { tz->broken = true; }
static int power_supply_get_property(struct power_supply *p, enum power_supply_property prop,
                                      union power_supply_propval *v);
static void startup_tests(void);
'''

TESTS = r'''
static int power_supply_get_property(struct power_supply *p, enum power_supply_property prop,
                                      union power_supply_propval *v)
{ return bq27xxx_battery_get_cached_property(p, &di, prop, v); }
static void startup_tests(void)
{
    struct device dev = {0};
    struct thermal_zone_device tz = {.psy = &psy, .recheck_delay_jiffies = THERMAL_RECHECK_DELAY};
    const enum power_supply_property props[] = {POWER_SUPPLY_PROP_STATUS,
        POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_VOLTAGE_NOW};
    for (int fail = 1; fail <= 5; fail++) {
        memset(&di, 0, sizeof(di));
        bq27xxx_battery_init_cache(&di.cache);
        di.dev = &dev; di.bat = &psy; di.cache_refresh_ms = 30000;
        refresh_warnings = refresh_debug = thermal_warnings = cache_notifications = 0;
        refresh_stage = fail; optional_error = 0;
        bq27xxx_battery_refresh_cache(&di);
        assert(!di.cache_ready && di.cache.flags == -EIO);
        assert(cache_delay == 1000 && refresh_warnings == 0 && refresh_debug == 1);
        for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
            int value = 12345678;
            assert(get(props[i], &value) == -ENODATA && value == 12345678);
            struct kobj_uevent_env env = {0}; char buf[64];
            assert(add_prop_uevent(&dev, &env, props[i], buf) == 0 && env.emitted == 0);
        }
        int temp = 12345678;
        for (int attempt = 0; attempt < 100; attempt++) {
            int ret = power_supply_read_temp(&tz, &temp);
            assert(ret == -EAGAIN && temp == 12345678);
            thermal_zone_recheck(&tz, ret);
        }
        assert(!thermal_warnings && !tz.broken && thermal_delay == THERMAL_RECHECK_DELAY);
        /* The first complete mandatory sample unlocks publication and regular polling. */
        refresh_stage = 0; optional_error = -EIO;
        jiffies += 1000;
        bq27xxx_battery_refresh_cache(&di);
        assert(di.cache_ready && cache_delay == 30000 && !refresh_warnings);
        assert(cache_notifications >= 2);
        assert(power_supply_read_temp(&tz, &temp) == 0 && temp == 28600);
        int value = -1;
        assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &value) == -ENODATA);
        assert(get(POWER_SUPPLY_PROP_STATUS, &value) == 0 && value == 3);
        assert(get(POWER_SUPPLY_PROP_PRESENT, &value) == 0 && value == 1);
        optional_error = 0; bq27xxx_battery_refresh_cache(&di);
        assert(get(POWER_SUPPLY_PROP_CHARGE_NOW, &value) == 0 && value == 3655000);
        /* After readiness, a genuine failed safety sample is still invalid and noisy. */
        refresh_stage = fail; bq27xxx_battery_refresh_cache(&di);
        assert(di.cache_ready && refresh_warnings == 1 && cache_delay == 30000);
        assert(get(POWER_SUPPLY_PROP_STATUS, &value) == -EIO);
        assert(power_supply_read_temp(&tz, &temp) == -EIO);
        tz.recheck_delay_jiffies = THERMAL_RECHECK_DELAY;
        thermal_zone_recheck(&tz, -EIO);
        assert(thermal_warnings == 1);
    }
    di.removed = true; cache_delay = 123;
    bq27xxx_battery_refresh_cache(&di);
    assert(cache_delay == 123); /* Removal cannot requeue the gauge. */
    puts("PASS: actual refresh/getter/uevent/thermal path;five first-sample faults;1-s retry;quiet thermal rechecks;optional errors;valid recovery;post-ready errors preserved");
}
'''


def main():
    source = (OUT / 'bq27xxx_battery.c').read_text()
    header = (OUT / 'bq27xxx_battery.h').read_text()
    core = (OUT / 'power_supply_core.c').read_text()
    thermal = (OUT / 'thermal_core.c').read_text()
    sysfs = (OUT / 'power_supply_sysfs.c').read_text()
    names = ('bq27xxx_cache_value', 'bq27xxx_battery_init_cache',
             'bq27xxx_battery_refresh_cache', 'bq27xxx_simple_value',
             'bq27xxx_cached_value', 'bq27xxx_battery_get_cached_property')
    functions = '\n\n'.join(prior.extract(source, name) for name in names)
    functions += '\n\n' + prior.extract(sysfs, 'add_prop_uevent')
    functions += '\n\n' + prior.extract(core, 'power_supply_read_temp')
    functions += '\n\n' + prior.extract(thermal, 'thermal_zone_recheck')
    props = sorted(set(re.findall(r'POWER_SUPPLY_PROP_[A-Z_]+', functions)))
    enums = 'enum power_supply_property {' + ','.join(props) + '};\n'
    structs = '\n'.join(re.search(r'struct ' + name + r'\s*\{.*?\n\};', header, re.S).group()
                        for name in ('bq27xxx_cached_value', 'bq27xxx_reg_cache'))
    structs += '''
struct delayed_work { int dummy; };
struct bq27xxx_device_info {
    struct mutex lock; struct bq27xxx_reg_cache cache; unsigned opts;
    bool cache_ready, removed; unsigned cache_refresh_ms; unsigned long last_update;
    struct power_supply *bat; struct device *dev;
    union power_supply_propval last_status; struct delayed_work work;
};
'''
    prelude = prior.PRELUDE
    start, tail = prelude.split('struct power_supply_attr {', 1)
    prelude = start + structs + 'struct power_supply_attr {' + tail + EXTRA
    attrs = 'static struct power_supply_attr power_supply_attrs[] = {\n' + '\n'.join(
        f'[{name}] = {{{{{name}}}, "{name.removeprefix("POWER_SUPPLY_PROP_")}"}},' for name in props) + '\n};\n'
    # The old cases represent a ready cache once they start installing measurements.
    old_tests = prior.TESTS.replace('    /* Successful values,', '    di.cache_ready = true;\n    /* Successful values,')
    old_tests = old_tests.replace('    return 0;\n}\n', '    startup_tests();\n    return 0;\n}\n')
    c = OUT / 'actual-battery-test.c'
    exe = OUT / 'actual-battery-test'
    # The real field is embedded; only its address crosses the queue mock boundary.
    c.write_text(enums + prelude + attrs + functions + old_tests + TESTS)
    cc = shutil.which('clang') or shutil.which('cc')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    for name in ('bq27xxx_battery.c', 'bq27xxx_battery.h', 'power_supply_core.c', 'thermal_core.c'):
        digest = hashlib.sha256((OUT / name).read_bytes()).hexdigest()
        print(name + ' SHA256: ' + digest)


if __name__ == '__main__':
    main()
