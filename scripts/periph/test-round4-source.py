#!/usr/bin/env python3
"""Host-only fault tests of the actual Round 4 Tristar ADC/mux functions.

No hardware is accessed. Driver functions and constants are extracted from
the source template, compiled with mocked transports and run under ASan and
UBSan. The voltage fixture is calculated independently with exact fractions.
"""
from fractions import Fraction
import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef int atomic_t;
struct device { int dummy; };
struct i2c_client { struct device dev; };
struct power_supply { int dummy; };
struct regmap { int dummy; };
struct device_node { int dummy; };
struct notifier_block { int dummy; };
struct delayed_work { int dummy; };
struct mutex { int dummy; };
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP,
       POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID };
struct usb_gadget_current_budget {
    const struct device_node *of_node;
    unsigned current_ma;
    bool configured, connected, suspended, host_grant;
};
#define min(a, b) ((a) < (b) ? (a) : (b))
static uint64_t div_u64(uint64_t n, unsigned d) { return n / d; }
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before, args...) \
({ int rc = -ETIMEDOUT; \
   for (unsigned n = 0; n < (timeout_us) / (sleep_us) + 2U; ++n) { \
       (val) = op(args); if (cond) { rc = 0; break; } \
   } rc; })
static void usleep_range(unsigned low, unsigned high)
{
    assert(low == 5000 && high == 6000);
}
static int apple_tristar_read(struct i2c_client *client, u8 reg);
static int i2c_smbus_write_byte_data(struct i2c_client *client, u8 reg, u8 value);
static int regmap_read(struct regmap *map, unsigned reg, unsigned *value);
static int regmap_write(struct regmap *map, unsigned reg, unsigned value);
static int regmap_bulk_read(struct regmap *map, unsigned reg, void *out, unsigned size);
'''


MOCKS = r'''
static struct apple_tristar ts;
static struct i2c_client client;
static struct regmap pmic;
static u8 route;
static unsigned adc_raw;
static int route_reads, route_writes, done_reads, trigger_writes, bulk_reads;
static int fail_route_read, fail_route_write, mismatch_route_write, route_write_mode;
static int fail_done_read, fail_trigger, fail_bulk, stale_reads, conversion_polls;
static bool conversion_active, conversion_done, never_done, invalid_result;
static bool stale_stuck, restore_blocked, observed_low;
enum { FAIL_BEFORE = 1, FAIL_AFTER };
static int apple_tristar_read(struct i2c_client *c, u8 reg)
{
    assert(c == &client && reg == TRISTAR_DX_CONTROL);
    ++route_reads;
    if (route_reads == fail_route_read) return -EIO;
    if (route_writes == mismatch_route_write && mismatch_route_write) {
        mismatch_route_write = 0;
        return route ^ 1;
    }
    return route;
}
static int i2c_smbus_write_byte_data(struct i2c_client *c, u8 reg, u8 value)
{
    assert(c == &client && reg == TRISTAR_DX_CONTROL);
    ++route_writes;
    bool fault = route_writes == fail_route_write;
    if (restore_blocked && value == ts.saved_route) return -EIO;
    if (fault && route_write_mode == FAIL_BEFORE) return -EIO;
    route = value;
    if (fault && route_write_mode == FAIL_AFTER) return -EIO;
    return 0;
}
static int regmap_read(struct regmap *m, unsigned reg, unsigned *value)
{
    assert(m == &pmic && reg == D2365_ADC_DONE);
    ++done_reads;
    if (done_reads == fail_done_read) return -EIO;
    if (!conversion_active) {
        if (stale_stuck || stale_reads-- > 0) *value = 1;
        else { *value = 0; observed_low = true; }
    } else if (never_done || conversion_polls-- > 0) {
        *value = 0;
    } else {
        *value = 1;
        conversion_active = false;
        conversion_done = true;
    }
    return 0;
}
static int regmap_write(struct regmap *m, unsigned reg, unsigned value)
{
    /* The selected actual functions must never write a mask or ACK. */
    assert(m == &pmic && reg == D2365_ADC_START && value == 1);
    assert(observed_low && !conversion_active);
    observed_low = false;
    ++trigger_writes;
    if (fail_trigger) return -EIO;
    conversion_active = true;
    conversion_done = false;
    return 0;
}
static int regmap_bulk_read(struct regmap *m, unsigned reg, void *out, unsigned size)
{
    assert(m == &pmic && reg == D2365_ADC_RESULT && size == 2);
    assert(conversion_done && !conversion_active);
    ++bulk_reads;
    if (fail_bulk) return -EIO;
    u8 *b = out;
    b[0] = (adc_raw & 15) | (invalid_result ? 0x80 : 0);
    b[1] = adc_raw >> 4;
    return 0;
}
static void reset_fixture(u8 initial_route)
{
    memset(&ts, 0, sizeof(ts));
    ts.client = &client; ts.pmic = &pmic;
    route = initial_route;
    adc_raw = 1800;
    route_reads = route_writes = done_reads = trigger_writes = bulk_reads = 0;
    fail_route_read = fail_route_write = mismatch_route_write = route_write_mode = 0;
    fail_done_read = fail_trigger = fail_bulk = stale_reads = conversion_polls = 0;
    conversion_active = conversion_done = never_done = invalid_result = false;
    stale_stuck = restore_blocked = observed_low = false;
}
'''


TESTS = r'''
int main(void)
{
    /* Actual arbitration: connected USB always wins over an analog signature. */
    struct usb_gadget_current_budget budget = {0};
    int type;
    assert(apple_tristar_budget(NULL, 2400000, &type) == 100000);
    assert(type == POWER_SUPPLY_USB_TYPE_UNKNOWN);
    budget.connected = true; budget.current_ma = 500;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 100000);
    assert(type == POWER_SUPPLY_USB_TYPE_SDP);
    budget.configured = true; budget.current_ma = 900;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 500000);
    budget.host_grant = true; budget.current_ma = 4000;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 2400000);
    budget.suspended = true;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 500000);
    budget.current_ma = 0; /* The real gadget snapshot zeros current on suspend. */
    assert(apple_tristar_budget(&budget, 2400000, &type) == 500000);
    budget.host_grant = false;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 500000);
    budget.connected = false;
    assert(apple_tristar_budget(&budget, 2400000, &type) == 2400000);
    assert(type == POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID);
    assert(apple_tristar_budget(&budget, 2100000, &type) == 2100000);
    assert(apple_tristar_budget(&budget, 4000000, &type) == 2400000);
    assert(apple_tristar_budget(&budget, 0, &type) == 500000);
    assert(type == POWER_SUPPLY_USB_TYPE_UNKNOWN);
    /* Exhaustive 12-bit values, against independently calculated Q16 fixtures. */
    for (unsigned raw = 0; raw < 4096; ++raw) {
        reset_fixture(0x91); adc_raw = raw;
        assert(apple_tristar_adc_mv(&ts) == expected_mv[raw]);
        assert(trigger_writes == 1 && bulk_reads == 1 && !ts.adc_poisoned);
    }
    const int signature[3] = {1500, 2200, 2880};
    const int expected_ma[3][3] = {{500, 1000, 1000}, {2100, 2400, 2400}, {1000, 1000, 1000}};
    for (int dp = 0; dp < 3; ++dp)
        for (int dm = 0; dm < 3; ++dm)
            assert(apple_tristar_brick_current(signature[dp], signature[dm]) == expected_ma[dp][dm] * 1000);
    assert(apple_tristar_brick_current(1499, 2200) == 0);
    assert(apple_tristar_brick_current(2200, 1499) == 0);
    assert(apple_tristar_brick_current(2199, 2199) == 500000);
    assert(apple_tristar_brick_current(2200, 2199) == 2100000);
    assert(apple_tristar_brick_current(2879, 2200) == 2400000);
    assert(apple_tristar_brick_current(2880, 2200) == 1000000);
    assert(apple_tristar_brick_current(4000, 4000) == 1000000);
    assert(apple_tristar_brick_current(4001, 2200) == -ERANGE);
    assert(apple_tristar_brick_current(2200, 4001) == -ERANGE);
    assert(apple_tristar_brick_current(-1, 2200) == -ERANGE);

    reset_fixture(0x91); stale_reads = 3; conversion_polls = 3;
    assert(apple_tristar_adc_mv(&ts) == expected_mv[adc_raw]);
    assert(done_reads == 8 && trigger_writes == 1);
    reset_fixture(0x91); stale_stuck = true;
    assert(apple_tristar_adc_mv(&ts) == -ETIMEDOUT && trigger_writes == 0);
    reset_fixture(0x91); fail_done_read = 1;
    assert(apple_tristar_adc_mv(&ts) == -EIO && trigger_writes == 0);

    /* Uncertain trigger, timeout and failed completion read permanently poison. */
    for (int fault = 0; fault < 3; ++fault) {
        reset_fixture(0x91);
        if (fault == 0) fail_trigger = 1;
        if (fault == 1) never_done = true;
        if (fault == 2) fail_done_read = 2;
        assert(apple_tristar_adc_mv(&ts) < 0 && ts.adc_poisoned);
        int reads_before = done_reads, writes_before = trigger_writes;
        never_done = false; fail_done_read = fail_trigger = 0;
        assert(apple_tristar_adc_mv(&ts) == -EIO);
        assert(done_reads == reads_before && trigger_writes == writes_before);
    }
    /* Completion is known before a failed/invalid data read: no false poison. */
    reset_fixture(0x91); fail_bulk = 1;
    assert(apple_tristar_adc_mv(&ts) == -EIO && !ts.adc_poisoned);
    fail_bulk = 0;
    assert(apple_tristar_adc_mv(&ts) == expected_mv[adc_raw] && trigger_writes == 2);
    reset_fixture(0x91); invalid_result = true;
    assert(apple_tristar_adc_mv(&ts) == -ENODATA && !ts.adc_poisoned);

    /* Both USB routes and both pins must restore the entire original byte. */
    const u8 routes[] = {0x91, 0x12};
    for (unsigned r = 0; r < 2; ++r)
        for (int dm = 0; dm < 2; ++dm) {
            reset_fixture(routes[r]);
            assert(apple_tristar_pin_mv(&ts, dm) == expected_mv[adc_raw]);
            assert(route == routes[r] && !ts.restore_pending && route_writes == 2);
        }
    reset_fixture(0x22);
    assert(apple_tristar_pin_mv(&ts, false) == -EOPNOTSUPP && !route_writes && !trigger_writes);

    int faults = 0;
    for (int phase = 1; phase <= 2; ++phase)
        for (int mode = FAIL_BEFORE; mode <= FAIL_AFTER; ++mode) {
            reset_fixture(0x91); fail_route_write = phase; route_write_mode = mode;
            assert(apple_tristar_pin_mv(&ts, false) < 0);
            assert(apple_tristar_restore_route(&ts) == 0);
            assert(route == 0x91 && !ts.restore_pending);
            ++faults;
        }
    for (int phase = 1; phase <= 2; ++phase) {
        reset_fixture(0x91); mismatch_route_write = phase;
        assert(apple_tristar_pin_mv(&ts, false) == -EIO);
        assert(apple_tristar_restore_route(&ts) == 0 && route == 0x91 && !ts.restore_pending);
        ++faults;
    }
    for (int phase = 1; phase <= 3; ++phase) {
        reset_fixture(0x91); fail_route_read = phase;
        assert(apple_tristar_pin_mv(&ts, false) == -EIO);
        assert(apple_tristar_restore_route(&ts) == 0 && route == 0x91 && !ts.restore_pending);
        ++faults;
    }
    reset_fixture(0x91); restore_blocked = true;
    assert(apple_tristar_pin_mv(&ts, false) == -EIO && ts.restore_pending);
    assert(ts.saved_route == 0x91 && route != 0x91);
    assert(apple_tristar_restore_route(&ts) == -EIO && ts.restore_pending);
    restore_blocked = false;
    assert(apple_tristar_restore_route(&ts) == 0 && route == 0x91 && !ts.restore_pending);
    /* ADC errors still restore routing and never become a voltage signature. */
    for (int fault = 0; fault < 4; ++fault) {
        reset_fixture(0x91);
        if (fault == 0) fail_trigger = 1;
        if (fault == 1) never_done = true;
        if (fault == 2) fail_bulk = 1;
        if (fault == 3) invalid_result = true;
        assert(apple_tristar_pin_mv(&ts, true) < 0);
        assert(route == 0x91 && !ts.restore_pending);
        ++faults;
    }
    printf("PASS: actual Tristar functions; source arbitration; 4096 voltage fixtures; brick thresholds; ADC freshness/poison; %d mux faults with exact restoration\n", faults);
    return 0;
}
'''


def main():
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('charging_test', root / 'scripts/periph/test-charging.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    driver = root / 'scripts/periph/kernel/apple_tristar_power.c'
    source = driver.read_text()
    constants = '\n'.join(re.findall(r'^#define (?:TRISTAR_|D2365_).*$', source, re.M))
    struct = re.search(r'struct apple_tristar\s*\{.*?\n\};', source, re.S)
    if not struct:
        raise ValueError('actual Tristar structure missing')
    names = ('apple_tristar_write_checked', 'apple_tristar_restore_route',
             'apple_tristar_pmic_read', 'apple_tristar_adc_mv',
             'apple_tristar_pin_mv', 'apple_tristar_brick_current', 'apple_tristar_budget')
    functions = '\n\n'.join(helper.function_text(source, name) for name in names)
    expected = []
    for raw in range(4096):
        volts = Fraction(40, 1000) + Fraction(raw * 5000, 4096 * 1000)
        q16 = int(volts * 65536)
        expected.append(str(int(Fraction(q16 * 1000, 65536))))
    fixture = 'static const int expected_mv[4096] = {' + ','.join(expected) + '};\n'
    output = root / 'build/periph/round4-source-test'
    output.mkdir(parents=True, exist_ok=True)
    c_file, executable = output / 'actual-source-test.c', output / 'actual-source-test'
    c_file.write_text(PRELUDE + constants + '\n' + struct.group() + '\n' + MOCKS + functions + fixture + TESTS)
    cc = shutil.which('clang') or shutil.which('cc')
    if not cc:
        raise SystemExit('host C compiler required')
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', str(c_file), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    digest = hashlib.sha256(driver.read_bytes()).hexdigest()
    (output / 'driver-sha256.txt').write_text(digest + '\n')
    print('Tristar source SHA256:', digest)


if __name__ == '__main__':
    main()
