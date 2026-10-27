/* SPDX-License-Identifier: GPL-2.0-only */
/* Production PMGR guard plus frozen state capture; no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include "pmgr-observe.h"

#define EHOSTDOWN 112
#define EPROBE_DEFER 517
#define EIO 5
#define AOP_REGS 5
#define AOP_DOMAINS 5
#define __iomem
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define APPLE_PMGR_PS_ACTUAL 0xf0
#define APPLE_PMGR_PS_TARGET 0x0f
#define APPLE_PMGR_PS_ACTIVE 0xf
#define APPLE_PMGR_RESET BIT(31)
#define APPLE_PMGR_DEV_DISABLE BIT(10)
#define APPLE_PMGR_PARENT_OFF BIT(11)
#define GENPD_STATE_ON 0
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define scnprintf snprintf

struct device_node { unsigned int index; };
struct device_driver { const char *name; };
struct device { struct device_driver *driver; unsigned int locked, refs; };
struct fake_regmap { u32 raw; int error; };
struct fake_genpd { unsigned int slock; int status; };
struct apple_pmgr_ps {
	struct fake_genpd genpd;
	struct fake_regmap *regmap;
	unsigned int offset;
};
struct platform_device { struct device dev; struct apple_pmgr_ps *ps; };
struct seq_file { void *private; char line[512]; };

static struct device_driver driver = { "apple-pmgr-pwrstate" };
static struct device_node nodes[AOP_DOMAINS];
static struct platform_device providers[AOP_DOMAINS];
static struct apple_pmgr_ps domains[AOP_DOMAINS];
static struct fake_regmap pmgr[AOP_DOMAINS];
static u32 words[AOP_REGS];
static unsigned int wrapper_reads[AOP_REGS], pmgr_reads, locks, irqs;
static bool state_probe;
static unsigned int attach_stage;
static int missing_provider = -1;

static struct platform_device *of_find_device_by_node(struct device_node *node)
{
	if ((int)node->index == missing_provider)
		return NULL;
	providers[node->index].dev.refs++;
	return &providers[node->index];
}
static void device_lock(struct device *dev) { assert(!dev->locked); dev->locked++; }
static void device_unlock(struct device *dev) { assert(dev->locked == 1); dev->locked--; }
static void put_device(struct device *dev) { assert(dev->refs == 1); dev->refs--; }
static struct apple_pmgr_ps *platform_get_drvdata(struct platform_device *p) { return p->ps; }
#define local_irq_save(flags) do { (flags) = irqs; assert(!irqs); irqs++; } while (0)
#define local_irq_restore(flags) do { assert(!locks); irqs = (flags); } while (0)
static void spin_lock_nested(unsigned int *lock, unsigned int i)
{
	assert(irqs && !*lock && locks == i);
	*lock = ++locks;
}
static void spin_unlock(unsigned int *lock)
{
	assert(irqs && *lock == locks);
	locks--;
	*lock = 0;
}
static int regmap_read(struct fake_regmap *map, unsigned int offset, u32 *value)
{
	assert(irqs && locks == AOP_DOMAINS && offset == 0);
	for (unsigned int i = 0; i < AOP_DOMAINS; i++)
		assert(providers[i].dev.locked && providers[i].dev.refs);
	pmgr_reads++;
	if (map->error)
		return map->error;
	*value = map->raw;
	return 0;
}
static u32 readl(const u32 *word)
{
	assert(irqs && locks == AOP_DOMAINS && pmgr_reads == AOP_DOMAINS);
	if (attach_stage == 1)
		assert(word >= &words[1] && word <= &words[4]);
	else
		assert(word == &words[1] || word == &words[3]);
	wrapper_reads[word - words]++;
	return *word;
}
static void seq_puts(struct seq_file *s, const char *line)
{
	snprintf(s->line, sizeof(s->line), "%s", line);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "state-probe-functions.h"
#pragma GCC diagnostic pop

static struct a9_aop aop;

static void reset(void)
{
	memset(&aop, 0, sizeof(aop));
	memset(wrapper_reads, 0, sizeof(wrapper_reads));
	pmgr_reads = locks = irqs = 0;
	missing_provider = -1;
	attach_stage = 0;
	for (unsigned int i = 0; i < AOP_DOMAINS; i++) {
		nodes[i].index = i;
		pmgr[i] = (struct fake_regmap){ .raw = 0xff };
		domains[i] = (struct apple_pmgr_ps){ .regmap = &pmgr[i] };
		providers[i] = (struct platform_device){ .dev.driver = &driver, .ps = &domains[i] };
		aop.domains[i] = &nodes[i];
		aop.regs[i] = &words[i];
		words[i] = 0xdeadbeef;
	}
	words[1] = words[3] = 0x00020001;
	words[2] = 0x00020001;
	words[4] = 0x79;
}

static void balanced(void)
{
	assert(!locks && !irqs);
	for (unsigned int i = 0; i < AOP_DOMAINS; i++)
		assert(!providers[i].dev.locked && !providers[i].dev.refs && !domains[i].genpd.slock);
}

static void check(bool wrapper, const char *result)
{
	assert(a9_aop_capture_state(&aop) == 0);
	assert(pmgr_reads == AOP_DOMAINS);
	for (unsigned int i = 0; i < AOP_REGS; i++)
		assert(wrapper_reads[i] == (wrapper && (i == 1 || i == 3)));
	assert(strstr(aop.state_line, result));
	assert(strchr(aop.state_line, '\n') == aop.state_line + strlen(aop.state_line) - 1);
	balanced();
}

static void check_attach(bool wrapper, const char *result)
{
	struct seq_file s = { .private = &aop };
	u32 saved[AOP_REGS];
	unsigned int reads;
	unsigned int expected = AOP_DOMAINS;

	memcpy(saved, words, sizeof(saved));
	attach_stage = 1;
	assert(apple_a9_aop_attach_stage() == 1);
	assert(a9_aop_capture_attach(&aop) == 0);
	for (unsigned int i = 0; i < AOP_DOMAINS; i++)
		if (pmgr[i].error) {
			expected = i + 1;
			break;
		}
	assert(pmgr_reads == expected);
	for (unsigned int i = 0; i < AOP_REGS; i++)
		assert(wrapper_reads[i] == (wrapper && i > 0));
	assert(!memcmp(saved, words, sizeof(saved)));
	assert(strstr(aop.attach_line, result));
	if (wrapper) {
		assert(strstr(aop.attach_line, "execution=unknown wrapper_b88="));
		assert(!strstr(aop.attach_line, "cpu_status="));
	}
	assert(strstr(aop.attach_line, "hardware_writes=none fifo_reads=none irqs=none dma=none\n"));
	assert(strlen(aop.attach_line) < sizeof(aop.attach_line) - 1);
	reads = pmgr_reads;
	for (unsigned int i = 0; i < 10; i++) {
		assert(!a9_aop_attach_status_show(&s, NULL));
		assert(!strcmp(s.line, aop.attach_line));
	}
	assert(pmgr_reads == reads);
	for (unsigned int i = 0; i < AOP_REGS; i++)
		assert(wrapper_reads[i] == (wrapper && i > 0));
	balanced();
}

int main(void)
{
	struct seq_file s = { .private = &aop };
	unsigned int reads;
	u32 bad[] = { 0x00, 0xf0, 0x4f, 0xff | APPLE_PMGR_RESET,
		      0xff | APPLE_PMGR_DEV_DISABLE, 0xff | APPLE_PMGR_PARENT_OFF };

	state_probe = false;
	assert(!apple_a9_aop_state_probe());
	state_probe = true;
	assert(apple_a9_aop_state_probe());
	reset();
	check(true, "wrapper_b88=0x00020001 b88=1 4020=1 -> powered execution=unknown\n");
	assert(words[1] == 0x20001 && words[3] == 0x20001);
	reads = pmgr_reads;
	words[1] = words[3] = pmgr[1].raw = 0;
	assert(a9_aop_state_show(&s, NULL) == 0);
	assert(pmgr_reads == reads && wrapper_reads[1] == 1 && wrapper_reads[3] == 1);
	assert(strstr(s.line, "-> powered execution=unknown\n")); /* Frozen, never reread at debugfs. */
	reset();
	words[1] = 0x20000;
	words[3] = 0x20000;
	check(true, "wrapper_b88=0x00020000 b88=0 4020=0 -> powered execution=unknown\n");
	reset();
	words[1] = 0x20000;
	check(true, "b88=0 4020=1 -> powered execution=unknown\n");
	for (unsigned int i = 0; i < AOP_DOMAINS; i++) {
		for (unsigned int j = 0; j < ARRAY_SIZE(bad); j++) {
			reset();
			pmgr[i].raw = bad[j];
			check(false, i == 1 && (bad[j] & 0xf0) == 0 ? "-> unpowered" : "-> unknown");
			assert(strstr(aop.state_line, "wrapper_b88=skipped b88=skipped 4020=skipped"));
			assert(pmgr[i].raw == bad[j]); /* No PMGR mutation, including faults/reset. */
		}
		reset();
		domains[i].genpd.status = 1;
		check(false, "-> unknown");
		reset();
		pmgr[i].error = -EIO;
		assert(a9_aop_capture_state(&aop) == 0);
		assert(!wrapper_reads[1] && !wrapper_reads[3]);
		assert(strstr(aop.state_line, "target=unknown actual=unknown"));
		balanced();
		reset();
		missing_provider = (int)i;
		assert(a9_aop_capture_state(&aop) == -EPROBE_DEFER);
		assert(!pmgr_reads && !wrapper_reads[1] && !wrapper_reads[3] && !aop.state_line[0]);
		balanced();
	}
	reset();
	pmgr[1].raw |= BIT(28); /* Existing automatic PM is recorded, never changed. */
	check(true, "-> powered execution=unknown");
	assert(pmgr[1].raw == (0xff | BIT(28)));
	reset();
	check_attach(true, "result=observed-mailbox-enabled-empty");
	reset();
	words[3] &= ~BIT(17);
	check_attach(true, "result=observed-pending-unconsumed");
	reset();
	words[1] &= ~1U;
	check_attach(true, "result=observed-mailbox-enabled-empty");
	reset();
	words[3] &= ~1U;
	check_attach(true, "result=aborted-outbox-disabled");
	for (unsigned int i = 2; i <= 3; i++) {
		for (unsigned int bit = 18; bit <= 19; bit++) {
			reset();
			words[i] |= BIT(bit);
			check_attach(true, "result=aborted-mailbox-fault");
		}
	}
	for (unsigned int i = 0; i < AOP_DOMAINS; i++) {
		for (unsigned int j = 0; j < ARRAY_SIZE(bad); j++) {
			reset();
			pmgr[i].raw = bad[j];
			check_attach(false, "result=aborted-domain-guard");
		}
		reset();
		domains[i].genpd.status = 1;
		check_attach(false, "result=aborted-domain-guard");
		reset();
		pmgr[i].error = -EIO;
		check_attach(false, "guard_errno=-5 wrapper=skipped");
		reset();
		attach_stage = 1;
		missing_provider = (int)i;
		assert(a9_aop_capture_attach(&aop) == -EPROBE_DEFER);
		assert(!pmgr_reads && !aop.attach_line[0]);
		for (unsigned int j = 0; j < AOP_REGS; j++)
			assert(!wrapper_reads[j]);
		balanced();
	}
	for (unsigned int stage = 2; stage <= 3; stage++) {
		reset();
		/* Deliberately provide no mappings or domain pointers. */
		memset(&aop, 0, sizeof(aop));
		attach_stage = stage;
		assert(!a9_aop_capture_attach(&aop));
		assert(strstr(aop.attach_line, "delegated-to-active-experiment wrapper=skipped"));
		assert(!pmgr_reads);
		for (unsigned int i = 0; i < AOP_REGS; i++)
			assert(!wrapper_reads[i]);
		balanced();
	}
	for (unsigned int stage = 0; stage <= 4; stage += 4) {
		reset();
		attach_stage = stage;
		assert(a9_aop_capture_attach(&aop) == -EINVAL);
		assert(!pmgr_reads && !aop.attach_line[0]);
	}
	puts("PASS: production PMGR/state probe; only b88/4020 after five powered-domain checks, execution unknown regardless of b88 bit0; off/unknown, faults/defer, frozen debugfs, balanced locks");
	puts("PASS: staged observer; four status reads, faults/off/defer, frozen debugfs; stages 2/3 delegate without observer MMIO");
	return 0;
}
