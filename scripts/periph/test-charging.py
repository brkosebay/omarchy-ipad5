#!/usr/bin/env python3
"""Compile actual J71t calibration/transaction functions against a faulted regmap.

This is a host-only test. It neither loads a kernel nor accesses a device.
Private CBAT data is passed to the test executable by local pathname and is
never included in its generated source or printed.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess


def function_text(source, name):
    match = re.search(r"^static (?:int|void|bool)\s+" + re.escape(name) + r"\s*\(", source, re.M)
    if not match:
        raise ValueError(f"missing actual driver function {name}")
    start = source.index("{", match.end())
    depth, state, escape, i = 0, "code", False, start
    while i < len(source):
        c = source[i]
        following = source[i:i + 2]
        if state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if following == "*/":
                state = "code"
                i += 1
        elif state in ('"', "'"):
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == state:
                state = "code"
        elif following == "//":
            state = "line"
            i += 1
        elif following == "/*":
            state = "block"
            i += 1
        elif c in ('"', "'"):
            state = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():i + 1]
        i += 1
    raise ValueError(f"unbalanced body for {name}")


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t s64;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define dev_info(...) test_dev_info(__VA_ARGS__)
#define dev_warn(...) ((void)0)
#define dev_crit(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
struct device { void *data; };
struct power_supply { void *data; };
union power_supply_propval { int intval; };
enum power_supply_property { POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
    POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_VOLTAGE_NOW };
struct mutex { int dummy; };
struct work_struct { int dummy; };
struct delayed_work { struct work_struct work; };
struct regmap { u8 reg[0xb00]; };
static int reschedules, notifications, policy_logs;
static unsigned long jiffies;
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define time_before(a, b) ((long)((a) - (b)) < 0)
static void test_dev_info(struct device *dev, const char *fmt, ...)
{
    (void)dev;
    if (!strncmp(fmt, "charging policy", 15)) ++policy_logs;
}
static unsigned scheduled_delay;
static void *system_power_efficient_wq;
static void mutex_lock(struct mutex *m) { (void)m; }
static void mutex_unlock(struct mutex *m) { (void)m; }
static unsigned msecs_to_jiffies(unsigned ms) { return ms; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void cancel_delayed_work_sync(struct delayed_work *work) { (void)work; }
static void msleep(unsigned ms) { assert(ms == 20); }
static void mod_delayed_work(void *q, struct delayed_work *work, unsigned delay)
{
    (void)q; (void)work; ++reschedules; scheduled_delay = delay;
}
static void power_supply_changed(struct power_supply *p) { (void)p; ++notifications; }
static void *power_supply_get_drvdata(struct power_supply *p) { return p->data; }
static s64 div_s64(s64 n, int d) { return n / d; }
static uint32_t get_unaligned_le32(const u8 *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put_le32(u8 *p, uint32_t n)
{
    for (int i = 0; i < 4; ++i) p[i] = n >> (8 * i);
}
static int reads, writes, guard_calls;
static int fail_read, fail_write, fault_mode, persistent_reg;
static int mismatch_reg, fail_guard, online_value, battery_error;
static int source_value, source_calls, fail_source, source_change_at, changed_source_value;
static bool checking_transactions, battery_available;
static int eligible_value, control_writes, write_regs[4096];
static int battery_present, battery_temp, battery_voltage;
static int battery_fail_prop;
enum { PRE_WRITE_ERROR = 1, POST_WRITE_ERROR, READBACK_MISMATCH };
static int regmap_read(struct regmap *map, unsigned reg, unsigned *out)
{
    assert(reg < sizeof(map->reg));
    ++reads;
    if (fail_read && reads == fail_read) return -EIO;
    *out = map->reg[reg];
    if (mismatch_reg == (int)reg) { *out ^= 1; mismatch_reg = -1; }
    return 0;
}
static int regmap_write(struct regmap *map, unsigned reg, unsigned value);
'''


MOCKS = r'''
static int j71t_online(struct j71t_charger *charger)
{
    (void)charger;
    ++guard_calls;
    return guard_calls == fail_guard ? -ENOLINK : online_value;
}
static int j71t_source_eligible(struct j71t_charger *charger)
{ (void)charger; return eligible_value; }
static struct power_supply battery;
static struct power_supply *power_supply_get_by_name(const char *name)
{
    assert(!strcmp(name, "battery"));
    return battery_available ? &battery : NULL;
}
static void power_supply_put(struct power_supply *psy) { assert(psy == &battery); }
static int power_supply_get_property(struct power_supply *psy, enum power_supply_property prop,
                                      union power_supply_propval *out)
{
    assert(psy == &battery);
    if (battery_error && (!battery_fail_prop || battery_fail_prop == (int)prop)) return battery_error;
    if (prop == POWER_SUPPLY_PROP_PRESENT) out->intval = battery_present;
    else if (prop == POWER_SUPPLY_PROP_TEMP) out->intval = battery_temp;
    else if (prop == POWER_SUPPLY_PROP_VOLTAGE_NOW) out->intval = battery_voltage;
    else assert(false);
    return 0;
}
static int j71t_source_limit(struct j71t_charger *charger)
{
    (void)charger;
    ++source_calls;
    if (source_calls == source_change_at) source_value = changed_source_value;
    return source_calls == fail_source ? -EIO : source_value;
}
static int regmap_write(struct regmap *map, unsigned reg, unsigned value)
{
    assert(reg < sizeof(map->reg));
    assert(value <= 255);
    ++writes;
    assert(writes < 4096); write_regs[writes] = reg;
    if (reg == J71T_INPUT_CONTROL) ++control_writes;
    if (checking_transactions && (reg == J71T_INPUT_LIMIT || reg == J71T_CHARGE_CURRENT))
        assert(!(map->reg[J71T_INPUT_CONTROL] & J71T_INPUT_SUSPEND));
    if (persistent_reg == (int)reg) return -EIO;
    bool fault = writes == fail_write;
    if (fault && fault_mode == PRE_WRITE_ERROR) return -EIO;
    map->reg[reg] = value;
    if (fault && fault_mode == POST_WRITE_ERROR) return -EIO;
    if (fault && fault_mode == READBACK_MISMATCH) mismatch_reg = reg;
    return 0;
}
static struct regmap hardware;
static struct j71t_charger charger;
static struct power_supply supply;
static u8 real_codes[4];
static const u8 baseline_control = 0xa4, baseline_input = 2, baseline_charge = 60;
static void reset_fixture(void)
{
    memset(&hardware, 0, sizeof(hardware));
    memset(&charger, 0, sizeof(charger));
    hardware.reg[J71T_INPUT_CONTROL] = baseline_control;
    hardware.reg[J71T_INPUT_LIMIT] = baseline_input;
    hardware.reg[J71T_CHARGE_CURRENT] = baseline_charge;
    hardware.reg[J71T_MAX_CHARGE_CURRENT] = J71T_CHARGE_CODE;
    charger.map = &hardware;
    supply.data = &charger;
    charger.psy = &supply;
    charger.input_code = 0xba;
    memcpy(charger.input_codes, real_codes, sizeof(real_codes));
    charger.charge_code = J71T_CHARGE_CODE;
    charger.budget_ua = J71T_REQUEST_UA;
    charger.calibrated = true; charger.max_limit_ua = 2400000;
    eligible_value = 1; control_writes = 0;
    reads = writes = guard_calls = fail_read = fail_write = fail_guard = 0;
    fault_mode = 0;
    online_value = 1;
    source_value = J71T_REQUEST_UA;
    source_calls = fail_source = source_change_at = 0;
    changed_source_value = 0;
    reschedules = notifications = 0;
    scheduled_delay = 0;
    battery_error = 0; battery_available = true;
    battery_present = 1; battery_temp = 250; battery_voltage = 3800000;
    battery_fail_prop = policy_logs = 0; jiffies = 0;
    persistent_reg = mismatch_reg = -1;
    checking_transactions = true;
}
static void check_baseline(void)
{
    assert(!charger.active && !charger.saved);
    assert(hardware.reg[J71T_INPUT_CONTROL] == baseline_control);
    assert(hardware.reg[J71T_INPUT_LIMIT] == baseline_input);
    assert(hardware.reg[J71T_CHARGE_CURRENT] == baseline_charge);
}
'''


TESTS = r'''
static void calibration_tests(const char *path)
{
    u8 cb[24], code;
    const int targets[] = {500, 1000, 2100, 2400};
    const u8 expected_real[] = {0x20, 0x47, 0xa0, 0xba};
    const int identity[5] = {100, 475, 1000, 2025, 2325};
    for (int i = 0; i < 5; ++i) put_le32(cb + i * 4, identity[i] * 65536);
    put_le32(cb + 20, 1);
    assert(j71t_calibrate(cb, sizeof(cb), &code) == 0 && code == 0xba);
    assert(j71t_calibrate(NULL, 0, &code) < 0);
    assert(j71t_calibrate_budget(NULL, 24, 500, &code) < 0);
    assert(j71t_calibrate_budget(cb, sizeof(cb), 499, &code) < 0);
    assert(j71t_calibrate_budget(cb, sizeof(cb), 2401, &code) < 0);
    assert(j71t_calibrate(cb, 23, &code) < 0);
    assert(j71t_calibrate(cb, 25, &code) < 0);
    put_le32(cb + 20, 2);
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    put_le32(cb + 20, 1);
    put_le32(cb, 0xffffffff);
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    put_le32(cb, 100 * 65536);
    put_le32(cb + 4, 100 * 65536);
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    put_le32(cb + 4, 475 * 65536);
    put_le32(cb + 16, 3001 * 65536);
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    put_le32(cb + 16, 2400 * 65536); /* slope >12Q */
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    put_le32(cb + 12, 1800 * 65536);
    put_le32(cb + 16, 2100 * 65536); /* valid slope, output exceeds budget */
    assert(j71t_calibrate(cb, sizeof(cb), &code) < 0);
    FILE *fp = fopen(path, "rb");
    assert(fp && fread(cb, 1, sizeof(cb), fp) == sizeof(cb) && fgetc(fp) == EOF);
    fclose(fp);
    assert(j71t_calibrate(cb, sizeof(cb), &code) == 0 && code == 0xba);
    for (unsigned i = 0; i < ARRAY_SIZE(targets); ++i) {
        assert(j71t_calibrate_budget(cb, sizeof(cb), targets[i], &real_codes[i]) == 0);
        assert(real_codes[i] == expected_real[i]);
        assert(75 + real_codes[i] * 12.5 <= targets[i]);
        assert(!i || real_codes[i] > real_codes[i - 1]);
    }
}


static void run_policy(void)
{
    j71t_policy_work(&charger.work.work);
    assert(notifications > 0);
}
static void tick(unsigned ms) { jiffies += ms; run_policy(); }
static int request_limit(int limit)
{
    const union power_supply_propval value = {.intval = limit};
    return j71t_set_property(&supply, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &value);
}
static void arm(void)
{
    charger.user_limit_ua = 2400000;
    source_value = 500000;
    run_policy();
    assert(charger.active && charger.budget_ua == 500000);
    tick(1000); assert(charger.budget_ua == 1000000);
    tick(1000); assert(charger.budget_ua == 2100000);
    tick(1000); assert(charger.budget_ua == 2400000);
    assert(hardware.reg[J71T_CHARGE_CURRENT] == 50);
    assert(hardware.reg[J71T_INPUT_LIMIT] == 0xba);
    assert(control_writes == 0);
}
static void policy_tests(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(j71t_budgets_ua); ++i) {
        assert(j71t_valid_limit(j71t_budgets_ua[i]) == j71t_budgets_ua[i]);
        assert(j71t_choose_budget(j71t_budgets_ua[i]) == (int)i);
    }
    assert(j71t_valid_limit(0) == 500000);
    assert(j71t_valid_limit(750000) == 500000);
    assert(j71t_valid_limit(0xffffffffU) == 500000);
    reset_fixture(); arm();
    int before = writes;
    for (int n = 0; n < 65; ++n) tick(1000);
    assert(writes == before && charger.active && !charger.faulted);
    /* Every write pair is battery first, then input, without a00 writes. */
    for (int n = 1; n <= writes; n += 2) {
        assert(write_regs[n] == J71T_CHARGE_CURRENT);
        assert(write_regs[n+1] == J71T_INPUT_LIMIT);
    }
    /* A reset dip holds for exactly 5 seconds, then steps down once. */
    source_value = 0; run_policy();
    tick(4999); assert(writes == before && charger.budget_ua == 2400000);
    tick(1); assert(charger.budget_ua == 500000 && charger.user_limit_ua == 2400000);
    assert(!charger.faulted && control_writes == 0);
    before = writes; tick(1000); assert(writes == before);
    source_value = 500000; tick(28000); assert(charger.budget_ua == 500000);
    tick(1000); assert(charger.budget_ua == 1000000);
    tick(1000); tick(1000); assert(charger.budget_ua == 2400000);
    /* Repeated short dips recover and rearm the grace, not a perpetual tick. */
    before = writes;
    for (int n = 0; n < 10; ++n) {
        source_value = 100000; run_policy(); tick(4999);
        source_value = 500000; run_policy();
    }
    assert(writes == before && charger.user_limit_ua == 2400000);
    eligible_value = 0; run_policy(); tick(4999);
    assert(writes == before && charger.budget_ua == 2400000);
    eligible_value = 1; run_policy();
    /* Real VBUS loss is the only automatic revocation and true zero target. */
    checking_transactions = false;
    online_value = 0; source_value = 0; run_policy();
    assert(!charger.active && !charger.saved && !charger.user_limit_ua);
    assert(hardware.reg[J71T_INPUT_CONTROL] & J71T_INPUT_SUSPEND);
    assert(hardware.reg[J71T_INPUT_LIMIT] == baseline_input);
    before = writes; run_policy(); assert(writes == before);
    online_value = 1; source_value = 500000; run_policy();
    assert(charger.active && !charger.user_limit_ua && charger.budget_ua == 500000);
    assert(!(hardware.reg[J71T_INPUT_CONTROL] & J71T_INPUT_SUSPEND));
    assert(request_limit(0) == 0 && charger.disabled && !charger.user_limit_ua);
    assert(hardware.reg[J71T_INPUT_CONTROL] & J71T_INPUT_SUSPEND);
    before = writes; tick(1000); assert(writes == before);
    assert(request_limit(2400000) == 0); run_policy();
    tick(1000); tick(1000); tick(1000);
    assert(charger.active && charger.budget_ua == 2400000);
    reset_fixture(); charger.max_limit_ua = 1000000;
    assert(request_limit(2400000) == -EINVAL);
    charger.user_limit_ua = 2400000; run_policy(); tick(1000); tick(1000);
    assert(charger.budget_ua == 1000000);
    reset_fixture(); eligible_value = 0; charger.user_limit_ua = 2400000;
    run_policy(); tick(1000); tick(1000); tick(1000);
    assert(charger.budget_ua == 500000); /* VBUS alone grants no elevated draw. */
    reset_fixture(); source_value = 100000; charger.user_limit_ua = 2400000;
    run_policy(); tick(1000); tick(1000); tick(1000);
    assert(charger.budget_ua == 500000);
    puts("PASS: direct iOS write order, steady 2400/2500, reset grace, one step-down, owner retention/rearm, DT ceiling, explicit zero and VBUS loss");
}
static void sample_tests(void)
{
    reset_fixture(); battery_present = 0; battery_voltage = 3900000; arm();
    assert(charger.active && !charger.faulted);
    battery_fail_prop = POWER_SUPPLY_PROP_VOLTAGE_NOW; battery_error = -EIO;
    int before = writes; run_policy(); tick(59999);
    assert(charger.active && writes == before && !charger.faulted);
    tick(1); assert(!charger.active && !charger.saved && !charger.faulted);
    assert(charger.user_limit_ua == 2400000 && control_writes == 0);
    before = writes; tick(1000); assert(writes == before);
    battery_error = 0; run_policy(); tick(30000); tick(1000); tick(1000);
    assert(charger.active && charger.budget_ua == 2400000);
    reset_fixture(); arm(); battery_available = false; run_policy(); tick(60000);
    assert(!charger.active && !charger.faulted && charger.user_limit_ua == 2400000);
    const int props[] = {POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TEMP,
                         POWER_SUPPLY_PROP_VOLTAGE_NOW};
    for (unsigned i = 0; i < ARRAY_SIZE(props); ++i) {
        reset_fixture(); battery_error = -ENODATA; battery_fail_prop = props[i];
        run_policy(); tick(65000); assert(!charger.active && !charger.faulted && !writes);
        battery_error = 0; arm(); assert(charger.active);
    }
    reset_fixture(); arm(); before = writes;
    fail_source = source_calls + 1; run_policy();
    assert(charger.active && writes == before && !charger.faulted);
    tick(1000); assert(charger.active && writes == before);
    online_value = -EIO; run_policy(); tick(59999);
    assert(charger.active && charger.user_limit_ua == 2400000);
    tick(1); assert(!charger.active && !charger.faulted);
    reset_fixture(); arm(); battery_voltage = 4200001; run_policy();
    assert(!charger.active && !charger.faulted && charger.user_limit_ua == 2400000);
    /* Persistent status/cap transport failures must not restart the 60 s timer. */
    reset_fixture(); arm(); before = writes;
    for (int n = 0; n <= 60; ++n) {
        fail_read = reads + 4; /* ownership triple succeeds; status read fails */
        tick(1000);
    }
    assert(!charger.active && !charger.faulted && writes > before);
    puts("PASS: PRESENT=0 plus valid voltage, failed gauge reads, supplier I/O, warmup, bounded 60 s unknown-sample grace and recovery");
}
static void thermal_fault_tests(void)
{
    reset_fixture(); arm();
    battery_temp = 449; run_policy(); assert(charger.budget_ua == 2400000);
    battery_temp = 450; run_policy();
    assert(charger.active && charger.budget_ua == 500000 && charger.thermal_limited);
    assert(charger.user_limit_ua == 2400000 && !charger.faulted && control_writes == 0);
    int before = writes;
    battery_temp = 421; tick(60000); assert(writes == before && charger.thermal_limited);
    battery_temp = 420; run_policy(); tick(1000); tick(1000);
    assert(!charger.thermal_limited && charger.budget_ua == 2400000);
    reset_fixture(); arm(); online_value = -EIO; battery_temp = 450; run_policy();
    assert(charger.active && charger.budget_ua == 500000 && charger.thermal_limited);
    assert(charger.user_limit_ua == 2400000 && !charger.faulted && control_writes == 0);
    int floor_writes = writes; run_policy(); assert(writes == floor_writes);
    reset_fixture(); arm(); online_value = -EIO; battery_voltage = 4200001; run_policy();
    assert(!charger.active && !charger.faulted && charger.user_limit_ua == 2400000);
    const int stops[] = {J71T_CHARGE_TIMEOUT, J71T_CHARGE_END};
    for (unsigned i = 0; i < ARRAY_SIZE(stops); ++i) {
        reset_fixture(); arm(); hardware.reg[J71T_STATUS] = stops[i]; run_policy();
        assert(charger.budget_ua == 500000 && !charger.faulted);
        before = writes; tick(60000); assert(writes == before);
        hardware.reg[J71T_STATUS] = 0; run_policy(); tick(1000); tick(1000);
        assert(charger.budget_ua == 2400000);
    }
    reset_fixture(); arm(); hardware.reg[J71T_INPUT_LIMIT] ^= 1; run_policy();
    assert(charger.faulted && !charger.active && !charger.saved);
    assert(charger.user_limit_ua == 2400000 && control_writes == 0);
    unsigned long deadline = charger.fault_until;
    before = writes; tick(59999); assert(writes == before && charger.faulted);
    /* A sysfs re-arm cannot bypass the ownership backoff. */
    assert(request_limit(2400000) == 0); run_policy();
    assert(charger.faulted && charger.fault_until == deadline && writes == before);
    tick(1); assert(!charger.faulted && charger.active);
    tick(1000); tick(1000); tick(1000); assert(charger.budget_ua == 2400000);
    reset_fixture(); arm();
    struct device dev = {.data = &charger};
    assert(j71t_quiesce(&dev) == 0 && charger.stopping);
    assert(charger.user_limit_ua == 2400000); check_baseline();
    assert(j71t_resume(&dev) == 0); run_policy(); assert(charger.active);
    puts("PASS: 45/42 C hysteresis, termination/timeout step-down, ownership-only 60 s latch, guarded automatic retry, PM snapshot restoration");
}
static void transaction_tests(void)
{
    int cases = 0;
    for (int mode = PRE_WRITE_ERROR; mode <= READBACK_MISMATCH; ++mode) {
        for (int at = 1; at <= 2; ++at) {
            reset_fixture();
            charger.budget_ua = 500000; charger.input_code = real_codes[0];
            charger.charge_code = J71T_USB_CHARGE_CODE;
            fail_write = at; fault_mode = mode;
            int result = j71t_enable(&charger);
            assert(result == (mode == READBACK_MISMATCH ? J71T_EREGREADBACK : -EIO));
            assert(charger.faulted == (mode == READBACK_MISMATCH));
            assert(j71t_restore(&charger) == 0); check_baseline();
            assert(control_writes == 0); ++cases;
        }
    }
    for (int at = 1; at <= 9; ++at) {
        reset_fixture(); charger.budget_ua = 500000;
        charger.input_code = real_codes[0]; charger.charge_code = J71T_USB_CHARGE_CODE;
        fail_read = at;
        assert(j71t_enable(&charger) == -EIO);
        assert(!charger.faulted);
        assert(j71t_restore(&charger) == 0); check_baseline(); ++cases;
    }
    for (int at = 1; at <= 2; ++at) {
        reset_fixture(); arm(); fail_write = writes + at; fault_mode = POST_WRITE_ERROR;
        assert(j71t_restore(&charger) == -EIO && charger.saved);
        assert(j71t_restore(&charger) == 0); check_baseline(); ++cases;
    }
    printf("PASS: %d injected transaction/rollback failures, no transport-error latch or nonzero input suspend\n", cases);
}
int main(int argc, char **argv)
{
    assert(argc == 2); calibration_tests(argv[1]);
    policy_tests(); sample_tests(); thermal_fault_tests(); transaction_tests();
    return 0;
}

'''


def main():
    root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--driver', type=Path, default=root / 'scripts/periph/kernel/apple_j71t_charger.c')
    ap.add_argument('--calibration', type=Path, default=root / 'build/firmware/private/CBAT.bin')
    args = ap.parse_args()
    source_bytes = args.driver.read_bytes()
    source = source_bytes.decode()
    digest = hashlib.sha256(source_bytes).hexdigest()
    constants = '\n'.join(re.findall(r'^#define J71T_.*$', source, re.M))
    structure = re.search(r'struct j71t_charger\s*\{.*?\n\};', source, re.S)
    if not structure:
        raise ValueError('missing actual charger structure')
    budgets = re.search(r'^static const int j71t_budgets_ua\[\].*?;', source, re.M | re.S)
    if not budgets:
        raise ValueError('missing actual source-budget table')
    functions = ('j71t_calibrate_budget', 'j71t_calibrate', 'j71t_choose_budget',
                 'j71t_read', 'j71t_write_checked', 'j71t_set_suspend',
                 'j71t_valid_limit', 'j71t_apply', 'j71t_is_fault', 'j71t_fault', 'j71t_check_owned',
                 'j71t_thermal_floor',
                 'j71t_battery_safe', 'j71t_restore', 'j71t_policy_limit', 'j71t_charge_code',
                 'j71t_revoke_override', 'j71t_guard', 'j71t_enable',
                 'j71t_policy_log', 'j71t_policy_work', 'j71t_set_property', 'j71t_quiesce', 'j71t_resume')
    body = '\n\n'.join(function_text(source, name) for name in functions)
    output = root / 'build/periph/round6-charging-tests'
    output.mkdir(parents=True, exist_ok=True)
    c_path = output / 'actual-driver-test.c'
    exe = output / 'actual-driver-test'
    c_path.write_text(PRELUDE + constants + '\n' + budgets.group() + '\n' +
                      structure.group() + '\n' + MOCKS + body + TESTS)
    compiler = shutil.which('clang') or shutil.which('cc')
    if not compiler:
        raise SystemExit('host C compiler required')
    subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=undefined,address', str(c_path), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(args.calibration.resolve())], check=True)
    (output / 'driver-sha256.txt').write_text(digest + '\n')
    if hashlib.sha256(args.driver.read_bytes()).hexdigest() != digest:
        raise SystemExit('Driver changed during tests; rerun against its latest version')
    print(f'Driver source SHA256: {digest}')


if __name__ == '__main__':
    main()
