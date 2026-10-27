#!/usr/bin/env python3
"""Compile the real D2365 hall IRQ/PM functions against a fault-injecting mock.

Usage: test-smart-cover.py drivers/gpio/gpio-apple-d2365.c [--cc clang]

The struct, the D2365_* constants and the functions below are taken from the
driver source as they are; only the kernel services are mocked. The bus model
is the one recovered from AppleD2365PMU: events at 0x140 (clear on read),
interrupt mask at 0x1c0 (set = masked), GPIO line N = event 0x30 + N.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

FUNCTIONS = (
    'd2365_irq_sync', 'd2365_irq_bus_lock', 'd2365_irq_bus_sync_unlock',
    'd2365_irq_mask', 'd2365_irq_unmask', 'd2365_irq_set_type',
    'd2365_irq_set_wake', 'd2365_irq_hold', 'd2365_irq_thread',
    'd2365_gpio_suspend', 'd2365_gpio_resume',
)


def extract_function(source, name):
    at = re.search(r'^static [^;{]*\b' + name + r'\(', source, re.M)
    if not at:
        raise SystemExit(f'{name} not found')
    brace = source.index('{', at.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at.start():end]


def extract_struct(source):
    at = source.index('struct d2365_gpio {')
    return source[at:source.index('\n};', at) + 3]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int irqreturn_t;
#define BIT(n) (1UL << (n))
#define IRQ_TYPE_NONE 0
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_BOTH 3
#define IRQ_HANDLED 1
#define HZ 1000UL
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define msecs_to_jiffies(n) (n)
#define READ_ONCE(x) (x)
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) < (size); (bit)++) if (*(addr) & BIT(bit))
#define set_bit(nr, addr) (*(addr) |= BIT(nr))
#define clear_bit(nr, addr) (*(addr) &= ~BIT(nr))
#define __set_bit(nr, addr) set_bit(nr, addr)
#define __clear_bit(nr, addr) clear_bit(nr, addr)
#define test_bit(nr, addr) (!!(*(addr) & BIT(nr)))
static int errs, warns;
#define dev_err(d, ...) ((void)(d), errs++)
#define dev_warn(d, ...) ((void)(d), warns++)
#define dev_err_ratelimited(d, ...) ((void)(d), errs++)
#define dev_dbg(d, ...) ((void)(d))
struct mutex { int locked; };
struct device { void *data; };
struct gpio_chip { struct device *parent; };
struct regmap { u8 reg[0x200]; };
struct irq_data { unsigned long hwirq; void *data; };
struct delayed_work { int pending; };
struct irq_domain { int unused; };
'''

MOCKS = r'''
static struct d2365_gpio gpio;
static struct regmap map;
static struct device pmu, dev;
static struct irq_domain domain;
static unsigned long jiffies;
static int system_dfl_wq;
static bool bus_up;
static int reads, writes, hard_wakes, parent_disabled, enables, syncs;
static int scheduled, cancels, child0, child10, parent_wake, parent_error;
static int io_error;
static unsigned long last_delay;

static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *irq_data_get_irq_chip_data(struct irq_data *d) { return d->data; }
static int regmap_bulk_write(struct regmap *m, unsigned int reg,
			     const void *val, size_t n)
{
	assert(bus_up);
	assert((reg == 0x1c6 && n == 2) || (reg == 0x1c0 && n == 30));
	writes++;
	if (io_error)
		return io_error;
	memcpy(m->reg + reg, val, n);
	return 0;
}
static int regmap_bulk_read(struct regmap *m, unsigned int reg, void *val,
			    size_t n)
{
	assert(bus_up && !gpio.suspended);
	assert(reg == 0x140 && n == 30);
	reads++;
	if (io_error)
		return io_error;
	memcpy(val, m->reg + reg, n);
	memset(m->reg + reg, 0, n);	/* clear on read */
	return 0;
}
static int irq_set_irq_wake(int irq, unsigned int on)
{
	assert(irq == 76);
	assert(!gpio.irq_lock.locked);	/* outside the bus lock */
	if (parent_error)
		return parent_error;
	parent_wake += on ? 1 : -1;
	assert(parent_wake >= 0);
	return 0;
}
static void pm_wakeup_hard_event(struct device *d) { assert(d == &pmu); hard_wakes++; }
static void disable_irq_nosync(int irq) { assert(irq == 76); parent_disabled++; }
static void enable_irq(int irq) { assert(irq == 76); parent_disabled--; enables++; }
static void synchronize_irq(int irq) { assert(irq == 76 && !gpio.irq_lock.locked); syncs++; }
static bool mod_delayed_work(int q, struct delayed_work *w, unsigned long delay)
{
	(void)q;
	assert(w == &gpio.irq_retry);
	last_delay = delay;
	scheduled++;
	return true;
}
static bool cancel_delayed_work_sync(struct delayed_work *w)
{
	assert(w == &gpio.irq_retry && !gpio.irq_lock.locked);
	cancels++;
	return false;
}
static unsigned int irq_find_mapping(struct irq_domain *d, unsigned long hw)
{
	assert(d == &domain);
	return hw;
}
static void handle_nested_irq(unsigned int irq)
{
	assert(!gpio.irq_lock.locked);	/* a child may take the bus lock */
	if (irq == 0x30)
		child0++;
	else {
		assert(irq == 0x3a);
		child10++;
	}
}
static void reset(void)
{
	memset(&gpio, 0, sizeof(gpio));
	memset(&map, 0xa5, sizeof(map));
	memset(map.reg + 0x140, 0, 30);
	memset(map.reg + 0x1c0, 0xff, 30);
	dev.data = &gpio;
	gpio.chip.parent = &dev;
	gpio.pmu = &pmu;
	gpio.regmap = &map;
	gpio.domain = &domain;
	gpio.parent_irq = 76;
	gpio.irq_ready = true;
	reads = writes = hard_wakes = parent_disabled = enables = syncs = 0;
	scheduled = cancels = child0 = child10 = parent_wake = parent_error = 0;
	io_error = errs = warns = 0;
	bus_up = true;
	jiffies = 10;
}
'''

TESTS = r'''
static void bus_op(struct irq_data *d, void (*op)(struct irq_data *))
{
	d2365_irq_bus_lock(d);
	op(d);
	d2365_irq_bus_sync_unlock(d);
}

static void wake(struct irq_data *d, unsigned int on)
{
	d2365_irq_bus_lock(d);
	assert(!d2365_irq_set_wake(d, on));
	d2365_irq_bus_sync_unlock(d);
}

int main(void)
{
	struct irq_data lid = { .hwirq = 0x30, .data = &gpio };
	struct irq_data aux = { .hwirq = 0x3a, .data = &gpio };

	reset();
	bus_op(&lid, d2365_irq_unmask);
	assert(map.reg[0x1c6] == 0xfe && map.reg[0x1c7] == 0xff);
	bus_op(&aux, d2365_irq_unmask);
	assert(map.reg[0x1c6] == 0xfe && map.reg[0x1c7] == 0xfb);
	bus_op(&lid, d2365_irq_mask);
	assert(map.reg[0x1c6] == 0xff && map.reg[0x1c7] == 0xfb);
	assert(map.reg[0x1c5] == 0xff && map.reg[0x1c8] == 0xff);
	assert(!d2365_irq_set_type(&lid, IRQ_TYPE_EDGE_BOTH));
	assert(!d2365_irq_set_type(&lid, IRQ_TYPE_NONE));
	assert(d2365_irq_set_type(&lid, IRQ_TYPE_EDGE_RISING) == -EINVAL);
	puts("PASS unmask clears only the hall bits of 0x1c6/0x1c7 (set = masked); other events stay masked");

	reset();
	writes = 0;
	assert(!d2365_irq_set_wake(&lid, 1) && !parent_wake && !writes);
	assert(!d2365_irq_set_wake(&lid, 1));	/* idempotent per line */
	d2365_irq_bus_sync_unlock((d2365_irq_bus_lock(&lid), &lid));
	assert(parent_wake == 1);
	wake(&aux, 1);
	assert(parent_wake == 2 && gpio.wake_enabled == D2365_HALL_LINES);
	wake(&lid, 0);
	wake(&lid, 0);
	assert(parent_wake == 1);
	wake(&aux, 0);
	assert(parent_wake == 0 && !gpio.wake_enabled);
	parent_error = -EIO;
	wake(&lid, 1);
	assert(errs == 1);	/* reported, and the child never fails suspend */
	puts("PASS wake references reach AP GPIO 76 from sync_unlock only, balanced, no I2C in set_wake");

	reset();
	gpio.irq_enabled = BIT(0);
	map.reg[0x146] = 0x01;
	map.reg[0x147] = 0x04;
	map.reg[0x150] = 0x80;
	assert(d2365_irq_thread(76, &gpio) == IRQ_HANDLED);
	assert(reads == 1 && !writes && child0 == 1 && !child10);
	assert(!map.reg[0x146] && !map.reg[0x147] && !map.reg[0x150]);
	puts("PASS one 30-byte read at 0x140 clears all events; a masked hall is not delivered");

	reset();
	gpio.irq_enabled = D2365_HALL_LINES;
	assert(d2365_irq_thread(76, &gpio) == IRQ_HANDLED);
	assert(reads == 1 && child0 == 1 && child10 == 1);
	puts("PASS nothing latched (cleared elsewhere): every enabled switch resyncs its level");

	reset();
	gpio.irq_enabled = D2365_HALL_LINES;
	gpio.wake_enabled = BIT(0);
	assert(!d2365_gpio_suspend(&dev));
	assert(gpio.suspended && syncs == 1 && cancels == 1);
	assert(map.reg[0x1c6] == 0xfe && map.reg[0x1c7] == 0xff);
	bus_up = false;
	writes = 0;
	map.reg[0x146] = 0x01;
	d2365_irq_thread(76, &gpio);
	assert(!reads && hard_wakes == 1 && gpio.irq_held && parent_disabled == 1);
	assert(!scheduled && map.reg[0x146] == 0x01);
	d2365_irq_thread(76, &gpio);	/* re-fire while held: one disable only */
	assert(parent_disabled == 1 && hard_wakes == 2);
	bus_op(&aux, d2365_irq_mask);
	bus_op(&aux, d2365_irq_unmask);
	assert(!writes);	/* bus callbacks honour the sleep gate */
	bus_up = true;
	assert(!d2365_gpio_resume(&dev) && !gpio.suspended && !gpio.irq_held);
	assert(!parent_disabled && enables == 1);
	assert(map.reg[0x1c6] == 0xfe && map.reg[0x1c7] == 0xfb);
	d2365_irq_thread(76, &gpio);
	assert(reads == 1 && child0 == 1 && !child10 && !map.reg[0x146]);
	puts("PASS asleep: only wake halls unmasked, no I2C before resume, latched event delivered after");

	reset();
	gpio.suspended = true;
	d2365_irq_thread(76, &gpio);
	assert(!hard_wakes && gpio.irq_held);
	puts("PASS an IRQ in the sleep window aborts the transition only when a hall is wake-enabled");

	reset();
	io_error = -EIO;
	for (int i = 0; i < 8; i++) {
		d2365_irq_thread(76, &gpio);
		gpio.irq_held = false;	/* the retry work re-enabled it */
		parent_disabled = 0;
	}
	assert(scheduled == 5 && gpio.retries == 5 && errs > 5);
	assert(last_delay == 100UL << 5);
	io_error = 0;
	d2365_irq_thread(76, &gpio);
	assert(gpio.retries == 0);
	puts("PASS bus faults back off at most five times; a good read restores the retry budget");

	reset();
	gpio.irq_held = true;
	gpio.retries = 5;
	io_error = -EIO;
	assert(!d2365_gpio_suspend(&dev) && gpio.suspended && warns == 2);
	io_error = 0;
	assert(!d2365_gpio_resume(&dev) && !gpio.irq_held && !gpio.retries);
	puts("PASS a held or failing event path never refuses suspend; resume re-arms it");

	reset();
	gpio.irq_burst = D2365_IRQ_BURST;
	d2365_irq_thread(76, &gpio);
	assert(!reads && gpio.irq_held && scheduled == 1);
	jiffies += 2 * HZ;
	gpio.irq_held = false;
	d2365_irq_thread(76, &gpio);
	assert(reads == 1 && gpio.irq_burst == 1);
	puts("PASS more than 64 IRQs in a second hold the line before they can flood I2C");

	reset();
	gpio.irq_ready = false;
	assert(!d2365_gpio_suspend(&dev) && !d2365_gpio_resume(&dev));
	assert(!writes && !reads && !syncs);
	puts("PASS without the opt-in event path, suspend/resume never touch the PMIC");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    source = args.source.read_text()
    # The i2c core owns the PMIC wakeup source (wakeup-source on pmic@3c),
    # and the PMIC never enables runtime PM: either call breaks the driver.
    for banned in ('device_init_wakeup', 'device_set_wakeup', 'pm_runtime_'):
        if banned in source:
            raise SystemExit(f'FAIL {banned} in the driver')
    print('PASS no PMIC wakeup-source or runtime-PM calls (-EEXIST / -EACCES on hardware)', flush=True)
    defines = '\n'.join(re.findall(r'^#define D2365_(?:[^\n]*\\\n)*[^\n]*',
                                   source, re.M))
    code = '\n'.join([PRELUDE, defines, extract_struct(source), MOCKS] +
                     [extract_function(source, n) for n in FUNCTIONS] + [TESTS])
    with tempfile.TemporaryDirectory(prefix='d2365-irq-') as tmp:
        path = Path(tmp) / 'test.c'
        path.write_text(code)
        binary = Path(tmp) / 'test'
        subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
