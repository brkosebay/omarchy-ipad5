/* SPDX-License-Identifier: GPL-2.0-only */
/* Production TX, status capture and mask writes against a fake MMIO FIFO. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "a9-aop-experiment.h"
#include "a9-aop-system.h"

#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((1U << ((h) + 1)) - 1) & ~((1U << (l)) - 1))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define EIO 5
#define EPERM 1
#define ESHUTDOWN 108
#define ETIMEDOUT 110
#define dev_info(...) ((void)0)
#define __iomem
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define readl_poll_timeout_atomic(addr, value, cond, delay, timeout) \
	((void)(delay), (void)(timeout), (value) = readl(addr), (cond) ? 0 : -ETIMEDOUT)

enum { PHASE_PROBE, PHASE_STREAMING, PHASE_FAILED, PHASE_REFUSED };
#include "mailbox-types.h"
struct a9_aop {
	u8 *wrapper;
	u8 *autoboot;
	unsigned int experiment_stage, reg_count, reg_dropped;
	unsigned int cold_run;
	int dev, phase, rx_lock, tx_lock;
	bool inline_rx, mailbox_unmasked, rx_halt, stopping;
	const char *experiment_step;
	u32 fault_control;
	u64 tx_words;
	struct a9_exp_regs reg_trace[A9_EXP_REG_TRACE];
};
static u8 wrapper[0x1c000];
static u32 auto_boot, cpu_control;
static unsigned int wake;
static bool quiesce, handoff;
static unsigned int writes32, writes64, traces;
static u32 written_offsets[8], written_values[8];
static u64 now, last_wire;
static bool instant_drain;
static bool fault_after_write;
static u32 control = 0x20001;
static u32 masks = 0x1111, mirrors = 0x1111;

static u32 readl(const void *p)
{
	size_t off = (uintptr_t)p - (uintptr_t)wrapper;
	if (p == &auto_boot)
		return auto_boot;
	assert(off == 0x44 || off == 0xb88 || off == 0x4000 || off == 0x4004 ||
	       off == 0xc00 || off == 0xc04 || off == 0x4008 || off == 0x4020 || off == 0x4040);
	switch (off) {
	case 0x44: return cpu_control;
	case 0xb88: return 0x20001;
	case 0x4008: return control;
	case 0x4020: return 0x20001;
	case 0x4000: case 0x4004: return masks;
	case 0xc00: case 0xc04: return mirrors;
	default: return 0;
	}
}
static u32 aop_rd(struct a9_aop *aop, u32 off) { return readl(aop->wrapper + off); }
static void writel(u32 value, void *p)
{
	size_t off = (uintptr_t)p - (uintptr_t)wrapper;
	if (p == &auto_boot) {
		assert(wake >= A9_EXP_WAKE_AUTOBOOT && value == 1);
		off = 0x600; auto_boot = value;
	} else if (off == 0x4004 || off == 0xc04) {
		assert(value == 1 || value == 0x1000 || value == 0x110 || value == 0x10);
		if (off == 0x4004) masks &= ~value;
		else mirrors &= ~value;
	} else {
		assert(off == 0x44 && value == (cpu_control | 0x10)); cpu_control = value;
	}
	assert(writes32 < ARRAY_SIZE(written_offsets));
	written_offsets[writes32] = off; written_values[writes32] = value;
	writes32++;
	if (fault_after_write) control |= BIT(18);
}
static void writeq(u64 wire, void *p)
{
	assert((u8 *)p - wrapper == 0x4010);
	last_wire = wire;
	writes64++;
	control = instant_drain ? 0x20001 : 1;
}
static u64 ktime_get_boottime_ns(void) { return ++now; }
static void usleep_range(unsigned int lo, unsigned int hi)
{
	assert(lo == 50 && hi == 100); now += 50000; control = 0x20001;
}
static bool a9_aop_dead(struct a9_aop *aop)
{
	return aop->phase == PHASE_FAILED || aop->phase == PHASE_REFUSED;
}
static void a9_aop_trace(struct a9_aop *aop, char dir, u64 word, int status)
{
	(void)aop; (void)word; (void)status;
	assert(dir == 'T'); traces++;
}
#include "mailbox-functions.h"

static struct a9_aop fresh(void)
{
	struct a9_aop aop = { .wrapper = wrapper, .experiment_stage = 2 };
	writes32 = writes64 = traces = 0; control = 0x20001;
	masks = mirrors = 0x1111; instant_drain = false;
	auto_boot = 0; cpu_control = 0x200;
	wake = 0;
	fault_after_write = false;
	return aop;
}
int main(void)
{
	struct a9_aop aop = fresh();
	assert(!strcmp(a9_exp_steps[0].name, "passive"));
	assert(!a9_aop_send(&aop, 0, 0x00200000000c000cULL));
	assert(writes64 == 1 && !writes32 && traces == 1 && aop.tx_words == 1);
	assert(last_wire == 0x00200000000c000cULL && aop.reg_count == 3);
	assert(aop.reg_trace[0].value[6] == 0x20001);
	assert(aop.reg_trace[1].value[6] == 1); /* Immediate FIFO acceptance. */
	assert(aop.reg_trace[2].value[6] == 0x20001); /* Later consumption. */
	aop = fresh(); instant_drain = true;
	assert(!a9_aop_send(&aop, 0, 0x00200000000c000cULL));
	assert(aop.reg_trace[1].value[6] == 0x20001); /* Cannot prove non-acceptance. */
	aop = fresh(); control = BIT(16);
	assert(a9_aop_send(&aop, 0, 0x00200000000c000cULL) == -ETIMEDOUT && !writes64);
	aop = fresh(); control = BIT(18);
	assert(a9_aop_send(&aop, 0, 0x00200000000c000cULL) == -EIO && !writes64);
	aop = fresh();
	assert(a9_aop_send(&aop, 0, 0x00100000000c000cULL) == -EPERM && !writes64);
	a9_aop_experiment_unmask(&aop);
	assert(writes32 == 4 && masks == 0x110 && mirrors == 0x110);
	assert(aop.mailbox_unmasked && aop.reg_count == 8);
	for (wake = 1; wake <= 5; wake++) {
		unsigned int selected = wake;
		aop = fresh(); wake = selected;
		if (wake >= A9_EXP_WAKE_AUTOBOOT) aop.autoboot = (u8 *)&auto_boot;
		a9_aop_experiment_wake(&aop);
		assert(writes32 == (wake == 5 ? 4U : wake == 3 ? 1U : 2U));
		assert(!writes64 && !traces && (masks & 0x1001) == 0x1001 && mirrors == masks);
		assert(written_offsets[0] == (wake <= 2 || wake == 5 ? 0x4004 : 0x44));
		assert(written_values[0] == (wake <= 2 || wake == 5 ? wake == 2 ? 0x10 : 0x110 : 0x210));
		if (wake <= 2 || wake == 5) assert(written_offsets[1] == 0xc04);
		if (wake >= 4) assert(written_offsets[writes32 - 1] == 0x600 && auto_boot == 1);
		assert(aop.reg_trace[0].auto_boot_valid == (wake >= 4));
	}
	aop = fresh(); wake = 1; aop.fault_control = BIT(18);
	a9_aop_experiment_wake(&aop); assert(!writes32);
	aop = fresh(); aop.phase = PHASE_FAILED; wake = 1;
	a9_aop_experiment_wake(&aop); assert(!writes32);
	aop = fresh(); wake = 5; aop.autoboot = (u8 *)&auto_boot; fault_after_write = true;
	a9_aop_experiment_wake(&aop);
	assert(writes32 == 1 && aop.fault_control && !auto_boot && cpu_control == 0x200);
	aop = fresh();
	for (unsigned int i = 0; i < A9_EXP_REG_TRACE + 10; i++)
		a9_aop_experiment_regs(&aop, "capacity");
	assert(aop.reg_count == A9_EXP_REG_TRACE && aop.reg_dropped == 10);
	aop = fresh(); aop.experiment_stage = 3; aop.cold_run = 1;
	assert(!a9_aop_send(&aop, 2, 0x0010100858000000ULL));
	aop.phase = PHASE_FAILED;
	assert(!a9_aop_send(&aop, 4, 0x0080000000000000ULL));
	assert(!a9_aop_send(&aop, 0, a9_mgmt_ping(2, 4)));
	assert(a9_aop_send(&aop, 0x21, a9_afk_word(A9_AFK_INIT_ACK)) == -ESHUTDOWN);
	aop.rx_halt = true;
	assert(a9_aop_send(&aop, 2, 0x0050000000000000ULL) == -ESHUTDOWN);
	/* The new AP word is private to the selected shutdown owner. */
	aop = fresh(); aop.experiment_stage = 3; aop.cold_run = 2;
	assert(a9_aop_send(&aop, 0, a9_sys_ap_quiesce()) == -EPERM);
	quiesce = true;
	assert(a9_aop_send(&aop, 0, a9_sys_ap_quiesce()) == -EPERM);
	aop.inline_rx = true; aop.stopping = true;
	assert(!a9_aop_send(&aop, 0, a9_sys_ap_quiesce()));
	assert(!a9_sys_shutdown_tx_allowed(1, a9_sys_ap_quiesce()));
	assert(!a9_sys_shutdown_tx_allowed(0, a9_mgmt_word(0xb, 0)));
	assert(!a9_sys_shutdown_tx_allowed(0, a9_mgmt_word(6, 0)));
	assert(!a9_sys_shutdown_tx_allowed(0, a9_mgmt_word(5, 1)));
	aop.rx_halt = true;
	assert(a9_aop_send(&aop, 0, a9_sys_ap_quiesce()) == -ESHUTDOWN);
	aop.inline_rx = true;
	assert(a9_aop_send(&aop, 0, a9_mgmt_ping(3, 4)) == -ESHUTDOWN);
	assert(a9_aop_send(&aop, 2, 0x0050000000000000ULL) == -ESHUTDOWN);
	/* aop_handoff=sleep is the other synchronous shutdown owner. */
	quiesce = false; handoff = true;
	aop = fresh(); aop.experiment_stage = 3; aop.cold_run = 2;
	assert(a9_aop_send(&aop, 0, a9_sys_ap_quiesce()) == -EPERM);
	aop.inline_rx = true; aop.stopping = true;
	assert(!a9_aop_send(&aop, 0, a9_sys_ap_quiesce()));
	/* The incoming wake220 and AP-on words are ordinary stage3 TX. */
	aop = fresh(); aop.experiment_stage = 3; aop.cold_run = 2;
	assert(!a9_aop_send(&aop, 0, 0x0060000000000220ULL));
	assert(!a9_aop_send(&aop, 0, 0x00b0000000000020ULL));
	handoff = false;
	puts("PASS production mailbox: TX readback/full/fault stop; isolated IOP mask/iOS CPU release/combined stores; no NMI; bounded status trace");
	return 0;
}
