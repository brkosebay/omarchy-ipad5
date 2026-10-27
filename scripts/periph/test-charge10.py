#!/usr/bin/env python3
"""Offline: execute actual charge10 policy functions with ASan/UBSan and fake I/O."""
import importlib.util
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prior', Path(__file__).with_name('test-charging.py'))
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)

EXTRA = r'''
#include "capture.h"
#define dev_warn_ratelimited(...) ((void)0)
#define dev_info_ratelimited(...) ((void)0)
enum { POWER_SUPPLY_PROP_USB_TYPE = 20 };
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP,
       POWER_SUPPLY_USB_TYPE_DCP, POWER_SUPPLY_USB_TYPE_CDP,
       POWER_SUPPLY_USB_TYPE_ACA, POWER_SUPPLY_USB_TYPE_C, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID };
static unsigned source_epoch;
static int source_type;
static int apple_tristar_attach_id(struct power_supply *p, unsigned *id)
{ (void)p; *id = source_epoch; return 0; }
static int power_supply_get_property_from_supplier(struct power_supply *p, int prop,
                                                   union power_supply_propval *v)
{ (void)p; assert(prop == POWER_SUPPLY_PROP_USB_TYPE); v->intval = source_type; return 0; }
'''

TESTS = r'''
static void run_policy(void) { j71t_policy_work(&charger.work.work); }
static void tick(unsigned ms) { jiffies += ms; run_policy(); }
static int request_limit(int ua)
{ const union power_supply_propval v = {.intval = ua};
  return j71t_set_property(&supply, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &v); }
static void reset(void)
{ reset_fixture(); source_epoch = 0; source_type = POWER_SUPPLY_USB_TYPE_SDP;
  charger.budget_ua = 0; charger.source = &supply; source_value = 500000; }
static void ramp(void)
{ run_policy(); for (int i = 0; i < 4; i++) tick(1000); }
static void arm(void) { ramp(); assert(charger.active && charger.budget_ua == 2400000); }
static void unplug(bool policy_observes)
{
    checking_transactions = false; /* Re-enabling a truly disabled input is legitimate. */
    online_value = 0; source_value = 0; source_epoch++;
    if (policy_observes) run_policy();
    online_value = 1;
}
static void source_matrix(void)
{
    const int types[] = { POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
                         POWER_SUPPLY_USB_TYPE_CDP, POWER_SUPPLY_USB_TYPE_DCP,
                         POWER_SUPPLY_USB_TYPE_ACA, POWER_SUPPLY_USB_TYPE_C };
    const int ceilings[] = {500000, 1000000, 1500000, 2100000, 2400000};
    for (unsigned t = 0; t < ARRAY_SIZE(types); t++) {
        for (unsigned c = 0; c < ARRAY_SIZE(ceilings); c++) {
            reset(); source_type = types[t]; source_value = ceilings[c]; ramp();
            int want = types[t] == POWER_SUPPLY_USB_TYPE_SDP ? 2400000 : ceilings[c];
            assert(charger.ceiling_ua == want);
            assert(charger.budget_ua == j71t_budgets_ua[j71t_choose_budget(want)]);
            assert(charger.budget_ua <= charger.ceiling_ua);
        }
    }
    for (int fast = 0; fast < 2; fast++) {
        reset(); arm(); assert(request_limit(1000000) == 0); run_policy();
        assert(charger.budget_ua == 1000000);
        unplug(!fast); source_value = 2100000;
        source_type = POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID; ramp();
        assert(!charger.user_limit_ua && charger.budget_ua == 2100000);
        unplug(!fast); source_value = 500000; source_type = POWER_SUPPLY_USB_TYPE_SDP;
        arm(); assert(!charger.user_limit_ua);
        unplug(!fast); source_value = 2100000; source_type = POWER_SUPPLY_USB_TYPE_ACA;
        ramp(); assert(charger.budget_ua == 2100000);
        unplug(!fast); source_value = 500000; source_type = POWER_SUPPLY_USB_TYPE_SDP;
        arm(); assert(charger.backoffs == 0);
    }
    reset(); arm(); assert(request_limit(1000000) == 0); run_policy();
    int before = writes;
    /* Link PM, reset and loss of descriptor budget keep the same physical source. */
    for (int i = 0; i < 100; i++) { source_value = i % 2 ? 0 : 100000; tick(1000); }
    assert(charger.user_limit_ua == 1000000 && charger.budget_ua == 1000000);
    assert(writes == before && source_epoch == charger.attach_id);
    /* A manual host role without ACA cannot draw from the iPad's own port. */
    checking_transactions = false;
    online_value = 0; run_policy(); assert(!charger.active);
    assert(charger.user_limit_ua == 1000000);
    online_value = 1; ramp(); assert(charger.budget_ua == 1000000);
    /* An override cannot raise a detected brick/ACA ceiling. */
    reset(); source_type = POWER_SUPPLY_USB_TYPE_ACA; source_value = 2100000;
    assert(request_limit(2400000) == 0); ramp(); assert(charger.budget_ua == 2100000);
    assert(request_limit(12345) == -EINVAL);
    assert(request_limit(0) == 0); assert(charger.disabled);
    unplug(true); source_type = POWER_SUPPLY_USB_TYPE_SDP; source_value = 500000;
    arm(); assert(!charger.disabled);
    reset(); source_type = POWER_SUPPLY_USB_TYPE_UNKNOWN; source_value = 2400000;
    ramp(); assert(charger.budget_ua == 500000);
    puts("PASS charge10: 30 source/ceiling cases; Mac-wall-Mac, fast replug, ACA-Mac, link PM/reset, role change, per-attach override");
}
static void backoff_tests(void)
{
    reset(); arm();
    const int steps[] = {2100000, 1000000, 500000};
    for (unsigned i = 0; i < ARRAY_SIZE(steps); i++) {
        hardware.reg[J71T_INPUT_LIMIT] = charger.input_code - 1;
        run_policy();
        assert(charger.active && !charger.faulted);
        assert(charger.budget_ua == steps[i]);
        assert(charger.backoffs == i + 1);
        assert(!strcmp(charger.backoff_cause, "pmic-input-reduced"));
        unsigned long until = charger.adaptive_after;
        tick(59999); assert(charger.budget_ua == steps[i]);
        assert(charger.adaptive_after == until);
    }
    /* Bounded one-step retry after hold, still no higher than a lower source. */
    source_value = 500000; source_type = POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID;
    tick(1); assert(charger.budget_ua == 500000);
    source_type = POWER_SUPPLY_USB_TYPE_SDP;
    tick(60000); assert(charger.budget_ua <= 2100000);
    unplug(false); source_value = 500000; arm();
    assert(!charger.adaptive_ua && !charger.backoff_cause && !charger.sample_pending);
    /* A non-default PMU reduction selects and asserts the calibrated floor. */
    tick(6000);hardware.reg[J71T_INPUT_LIMIT] = 3; run_policy();
    assert(charger.budget_ua == 500000 && hardware.reg[J71T_INPUT_LIMIT] == charger.input_codes[0]);
    /* An upward setting change remains an ownership fault, not a back-off. */
    reset(); arm(); hardware.reg[J71T_INPUT_LIMIT]++;
    run_policy(); assert(charger.active); /* one existing bounded reassert */
    hardware.reg[J71T_INPUT_LIMIT]++; run_policy();
    assert(charger.faulted && !charger.active);
    puts("PASS charge10: PMU reduction ladder, 60 s hold/retry, lower-source cap, floor, epoch reset and ownership faults");
}
static void guard_tests(void)
{
    reset(); arm(); battery_temp = 450; run_policy();
    assert(charger.budget_ua == 500000 && charger.thermal_limited);
    battery_temp = 430; tick(60000); assert(charger.budget_ua == 500000);
    battery_temp = 420; tick(1000); tick(1000); tick(1000);
    assert(charger.budget_ua == 2400000);
    battery_voltage = 4400001; run_policy(); assert(!charger.active);
    reset(); arm(); battery_fail_prop = POWER_SUPPLY_PROP_VOLTAGE_NOW; battery_error = -EIO;
    run_policy(); tick(59999); assert(charger.active);
    tick(1); assert(!charger.active && !charger.faulted);
    reset(); arm(); online_value = -EIO; battery_temp = 450; run_policy();
    assert(charger.budget_ua == 500000);
    reset(); battery_error = -ENODATA; ramp(); assert(!charger.active && !writes);
    battery_error = 0; arm();
    puts("PASS charge10: thermal hysteresis, 4.4 V guard, missing gauge, transport grace and hot-pack precedence");
}

static void attach_reset_tests(void)
{
    reset();run_policy();assert(charger.budget_ua==500000);
    /* Captured 275.801 -> 275.827: default reset 26 ms after first write. */
    hardware.reg[J71T_INPUT_LIMIT]=0x02;tick(CAPTURE_ATTACH_RESET_MS);
    assert(charger.active && charger.attach_resets==1 && !charger.backoffs);
    assert(!charger.adaptive_ua && hardware.reg[J71T_INPUT_LIMIT]==charger.input_codes[0]);
    for(int i=0;i<3;i++){
        hardware.reg[J71T_INPUT_LIMIT]=0x02;tick(CAPTURE_ATTACH_RESET_MS);
        assert(!charger.faulted && !charger.backoffs && charger.budget_ua==500000);
        assert(hardware.reg[J71T_INPUT_LIMIT]==charger.input_codes[0]);
    }
    source_type=POWER_SUPPLY_USB_TYPE_CDP;source_value=1500000;tick(1000);
    assert(charger.budget_ua==1000000 && hardware.reg[J71T_INPUT_LIMIT]==charger.input_codes[1]);
    source_type=POWER_SUPPLY_USB_TYPE_C;source_value=3000000;
    tick(1000);tick(1000);assert(charger.budget_ua==2400000 && !charger.backoffs);
    /* Fast swap: epoch can change without ONLINE going low. */
    assert(request_limit(1000000)==0);run_policy();source_epoch++;
    source_type=POWER_SUPPLY_USB_TYPE_UNKNOWN;source_value=500000;
    hardware.reg[J71T_INPUT_LIMIT]=0x02;run_policy();
    assert(!charger.user_limit_ua && !charger.adaptive_ua && charger.budget_ua==500000);
    assert(hardware.reg[J71T_INPUT_LIMIT]==charger.input_codes[0]);
    tick(6000);hardware.reg[J71T_INPUT_LIMIT]=0x02;tick(CAPTURE_ATTACH_RESET_MS);
    assert(charger.attach_resets==1 && !charger.backoffs && !charger.faulted);
    puts("PASS charge10 captured policy: 26ms attach-default resets repaired without backoff, repeated defaults, CDP then 3A CC ramp, fast-swap epoch revokes caps and reasserts floor");
}


static void bc_to_computer_policy(void)
{
 reset();source_type=POWER_SUPPLY_USB_TYPE_CDP;source_value=1500000;ramp();
 assert(charger.ceiling_ua==1500000 && charger.budget_ua==1000000);
 unsigned id=charger.attach_id;
 source_type=POWER_SUPPLY_USB_TYPE_SDP;source_value=500000;tick(1000);tick(1000);
 assert(charger.attach_id==id && charger.ceiling_ua==2400000 && charger.budget_ua==2400000);
 assert(hardware.reg[J71T_INPUT_LIMIT]==charger.input_codes[3]);
 reset();source_type=POWER_SUPPLY_USB_TYPE_DCP;source_value=1500000;ramp();
 assert(request_limit(1000000)==0);run_policy();
 source_type=POWER_SUPPLY_USB_TYPE_SDP;source_value=100000;tick(1000);tick(1000);
 assert(charger.ceiling_ua==2400000 && charger.budget_ua==1000000 && charger.user_limit_ua==1000000);
 puts("PASS charge10 policy replay: early BC 1500/nominal1000 -> same-epoch SDP/board2400, calibrated 2400 reached; explicit user cap still wins");
}

int main(int argc, char **argv)
{
    assert(argc == 2); calibration_tests(argv[1]);
    source_matrix();attach_reset_tests();bc_to_computer_policy();
    reset(); source_type=POWER_SUPPLY_USB_TYPE_C; source_value=3000000; ramp();
    assert(charger.ceiling_ua==2400000 && charger.budget_ua==2400000);
    unplug(false); source_type=POWER_SUPPLY_USB_TYPE_C; source_value=1500000; ramp();
    assert(charger.ceiling_ua==1500000 && charger.budget_ua==1000000);
    puts("PASS charge10: Type-C 3A bounded to board 2.4A; 1.5A uses calibrated 1A rung");
    backoff_tests(); guard_tests(); transaction_tests();
    return 0;
}
'''


def main():
    source = (ROOT / 'scripts/periph/kernel/apple_j71t_charger.c').read_text()
    constants = '\n'.join(re.findall(r'^#define J71T_.*$', source, re.M))
    structure = re.search(r'struct j71t_charger\s*\{.*?\n\};', source, re.S).group()
    budgets = re.search(r'^static const int j71t_budgets_ua\[\].*?;', source, re.M | re.S).group()
    funcs = ('j71t_calibrate_budget', 'j71t_calibrate', 'j71t_choose_budget',
             'j71t_read', 'j71t_write_checked', 'j71t_set_suspend', 'j71t_valid_limit',
             'j71t_apply', 'j71t_is_fault', 'j71t_fault', 'j71t_check_owned',
             'j71t_thermal_floor', 'j71t_battery_safe', 'j71t_restore',
             'j71t_source_eligible', 'j71t_policy_limit',
             'j71t_charge_code', 'j71t_revoke_override', 'j71t_new_attach', 'j71t_guard',
             'j71t_enable', 'j71t_reassert', 'j71t_input_reduced', 'j71t_policy_log',
             'j71t_policy_work', 'j71t_set_property')
    mocks = prior.MOCKS.replace(prior.function_text(prior.MOCKS, 'j71t_source_eligible'), '')
    prelude = prior.PRELUDE
    # Prior fixture includes unused lifecycle stubs; compile warnings stay enabled
    # for extracted driver code, while only those mock helpers are marked unused.
    prelude = prelude.replace('static void *dev_get_drvdata', 'static __attribute__((unused)) void *dev_get_drvdata')
    for name in ('cancel_delayed_work_sync', 'msleep'):
        prelude = prelude.replace('static void ' + name, 'static __attribute__((unused)) void ' + name)
    body = '\n\n'.join(prior.function_text(source, f) for f in funcs)
    retained = '\n'.join(prior.function_text(prior.TESTS, f) for f in ('calibration_tests', 'transaction_tests'))
    out = ROOT / 'build/charge10/tests'
    out.mkdir(parents=True, exist_ok=True)
    c = out / 'policy.c'
    c.write_text(prelude + EXTRA + constants + '\n' + budgets + '\n' + structure +
                 mocks + body + retained.replace('static void transaction_tests',
                 'static void arm(void);\nstatic void transaction_tests') + TESTS)
    exe = out / 'policy'
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(ROOT / 'build/firmware/private/CBAT.bin')], check=True)


if __name__ == '__main__':
    main()
