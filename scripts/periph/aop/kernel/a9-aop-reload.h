/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J71t AOP reload/wake experiments, both opt-in and default off. Constants
 * and evidence types only; the MMIO lives in apple-a9-aop-active.c.
 * Evidence: docs/periph/ds-aop-reload-crash-and-wake.md.
 *
 * aop_spi0_reset=1: every halt + SRAM reload of a live AOP crashed in
 * AOPSPIMaster_channelFaultIRQ (0102f252, DFAR 0x4c): the AOP-private SPI0
 * controller kept channel 3's +0x8d0 bit 6 while the fresh firmware's
 * channel table was still empty. With the CPU halted, the AP replays the
 * firmware's own controller reset AOPSPIMaster_reset (0102f2f4), then the
 * firmware's own channel-3 teardown, and samples bit 6 for >= 150 ms after
 * the image copy before it releases RUN once. Round 4: the old session's
 * SPI0 global IRQ 0x10014 stays latched in the AOP interrupt controller
 * (SCMErr:0x3,3 at the accel STARTEP), so the AP disables that source before
 * the reset and acknowledges it after the clean window, just before RUN.
 * Wake7: the channel-3 idle wait gets the firmware's own 10 ms bound twice
 * over, sampled per 1 ms slice, and the disable is judged by the INTC mask
 * readback, which the firmware itself reads.
 * Wake8: after the targeted ack, replay INTC event 3 (+0x808: 1/read/0/read).
 * This resets retained interrupt delivery, not the read-only SCM state.
 *
 * aop_wake_wrapper=1: before 0x220, the AppleA7IOP status-3 resume wrapper
 * writes (b4b854..b4b8b4). No CPU RUN, no auto-boot, no NMI.
 */
#ifndef _A9_AOP_RELOAD_H
#define _A9_AOP_RELOAD_H

#include <linux/string.h>
#include <linux/types.h>
#include "a9-aop-handoff.h"

/* Firmware device spi0: IOP 0x10100000 (space 2, 0x1000) via /arm-io ranges. */
#define A9_SPI0_PA		0x210100000ULL
#define A9_SPI0_SIZE		0x1000U
#define A9_SPI0_WRAPPER_PA	0x210800000ULL	/* the only layout this applies to */
#define A9_SPI0_CONTROL		0x000U
#define A9_SPI0_CLOCK		0x008U	/* 0102f2aa programs 0x01b71b00 here */
#define A9_SPI0_FAULT		0x8d0U	/* bit 2n = channel n, read by 0102f20a */
#define A9_SPI0_FAULT_8D4	0x8d4U
#define A9_SPI0_CH3		0x280U	/* base + 0x100 + 0x80 * 3 */
#define A9_SPI0_CH3_0C		0x28cU
#define A9_SPI0_RESET		0x100U	/* control bit 8 */
#define A9_SPI0_ENABLE		0x001U	/* control bit 0 */
#define A9_SPI0_CH3_PENDING	0x040U
/* The firmware spins without bound; the AP gives each poll 1 ms. */
#define A9_SPI0_POLL_US		1000U
#define A9_SPI0_DELAY_US	100U	/* 0102b3f8(100, 0), twice */
#define A9_SPI0_REGS		6

static const u32 a9_spi0_offsets[A9_SPI0_REGS] = {
	A9_SPI0_CONTROL, A9_SPI0_CLOCK, A9_SPI0_FAULT, A9_SPI0_FAULT_8D4,
	A9_SPI0_CH3, A9_SPI0_CH3_0C,
};

/* Snapshot indices into a9_spi0_offsets. */
enum { A9_SPI0_I_CONTROL, A9_SPI0_I_CLOCK, A9_SPI0_I_FAULT };

/*
 * Channel-3 teardown after the reset (ds-aop-spi0-ch3-round3.md section 5).
 * The reset leaves C3 = 0x00010447: idle but still enabled, async-armed and
 * with the class-0 fault IRQ on, so a level source re-latched +0x8d0 bit 6
 * within ~105 ms with the CPU halted. AOPSPIMaster_teardownChannel
 * (0102e4f8) stops the channel and masks both IRQ classes; the fault
 * handler (0102f20a) then acknowledges with C3C bit 5.
 */
#define A9_SPI0_CH3_ROUTE	0x284U
#define A9_SPI0_CH3_RATE	0x288U
#define A9_SPI0_CH3_DESC	0x298U	/* channel +0x18, zeroed on teardown */
#define A9_SPI0_CH3_RUN		0x001U	/* C3 bit 0, channel run/enable */
#define A9_SPI0_CH3_IDLE	0x002U	/* C3 bit 1, polled after the stop */
#define A9_SPI0_CH3_IRQ0	0x040U	/* C3 bit 6, +0x8d0 class-0/fault enable */
#define A9_SPI0_CH3_IRQ1	0x100U	/* C3 bit 8, +0x8d4 class-1 enable */
#define A9_SPI0_CH3_STOP	0x411U	/* clear run bit 0 and bits 4, 10 */
#define A9_SPI0_CH3_CLASS0	0x450U	/* mask class 0: bits 4, 6, 10 */
#define A9_SPI0_CH3_CLASS1	0x510U	/* mask class 1: bits 4, 8, 10 */
#define A9_SPI0_CH3_ACK		0x020U	/* C3C bit 5, ORed by 0102f20a; self-clearing */
/*
 * AOPSPIMaster_setChannelEnabled(false) (0102ef7c) stores C3 & ~0x411 only
 * if bit 0 was set, then spins on bit 1 until master +0x30, and panics
 * "SCMto:%u" (0x010398da) past it. Event 1 sets +0x30 to 0x2710 us when it
 * is 0 (0102e6fa, as in the live-cold object), through timer +0x18 =
 * us * Hz / 1000000 (0102ff3c): a 10 ms bound. The AP polls in 1 ms slices,
 * up to twice that, sampling C3, the SCM state and +0x8d0 after each slice.
 */
#define A9_SPI0_CH3_IDLE_FW_US	10000U
#define A9_SPI0_CH3_SLICE_US	1000U
#define A9_SPI0_CH3_SLICES	20U	/* 20 ms */
/* At least 150 ms (the 16:27 re-latch took ~105 ms) of samples before RUN. */
#define A9_SPI0_WINDOW_SAMPLES	31U
#define A9_SPI0_WINDOW_US	5000U	/* 30 sleeps >= 5 ms each */
#define A9_SPI0_WINDOW_MS	150U	/* required first-to-last sample span */
#define A9_SPI0_CH3_REGS	7

static const u32 a9_spi0_ch3_offsets[A9_SPI0_CH3_REGS] = {
	A9_SPI0_CH3, A9_SPI0_CH3_ROUTE, A9_SPI0_CH3_RATE, A9_SPI0_CH3_0C,
	A9_SPI0_CH3_DESC, A9_SPI0_FAULT, A9_SPI0_FAULT_8D4,
};

/* Snapshot indices into a9_spi0_ch3_offsets. */
enum {
	A9_CH3_I_C3, A9_CH3_I_ROUTE, A9_CH3_I_RATE, A9_CH3_I_C3C, A9_CH3_I_DESC,
	A9_CH3_I_8D0, A9_CH3_I_8D4,
};

/* One idle-poll slice: C3 as the slice ended, then SCM state and +0x8d0. */
struct a9_aop_ch3_slice {
	u64 ns;				/* since the stop store */
	u32 c3, scm_lo, scm_hi, fault;
};

struct a9_aop_spi0_ch3 {
	u32 before[A9_SPI0_CH3_REGS], after[A9_SPI0_CH3_REGS];
	u32 stop, idle, class0, class1, ack;	/* words written / polled */
	int poll;			/* idle-bit poll result */
	bool run_before;		/* C3 bit 0 before the stop */
	unsigned int slices;		/* idle-poll slices used */
	struct a9_aop_ch3_slice slice[A9_SPI0_CH3_SLICES];
	u64 idle_us;			/* stop store to the idle read */
	int error;
	bool ran;			/* the guarded teardown executed */
	const char *failed;		/* distinct refusal reason, or NULL */
	/* Pre-RUN window after the image copy; a refusal also sets failed. */
	unsigned int samples;
	u64 elapsed_ms;
	bool window_done, reasserted;
	unsigned int reassert_sample;
	u64 reassert_ms;
	u32 reassert[A9_SPI0_CH3_REGS];
};

/*
 * AOP interrupt controller, wrapper offsets (ds-aop-spi0-ch3-round4.md
 * section 3). IRQ 0x10014 is group 1, source 0x14: bank 0, bit 20. The
 * group-1 disable/enable banks are write-one commands whose reads return
 * the mask, 1 = disabled: the sleep command saves +0xa00..+0xa08 (0102c5ce)
 * before masking all (0102c5da), the restore writes saved to +0xa00 and
 * ~saved to +0xa80 (0102c9e6/0102c9f4), and aop_interrupt_write_enabled
 * reads the target bank for the previous state (0102c9c0). So the disable
 * is judged by bit 20 of the +0xa00 readback (wake6: ffffffff before the
 * store, every source already masked by the quiesce). +0x81c only reports
 * an unmasked pending source: the sleep path masks all, writes group 7
 * +0xc020/+0xc024 = 2 and spins until +0x81c reads 0 (0102c654), so
 * all-ones there means the AP is not reading the INTC. +0x808 ends 0 after
 * the event-3 pulse. +0x900 is the group-1 raise (0102c7c6) and +0x980 its
 * pre-handler clear (0102c6f2); the firmware never reads them, so their
 * readbacks are logged only. No register shows a masked source pending.
 */
#define A9_INTC_RESET		0x808U	/* event-3 pulse 1 then 0 (0102c592) */
#define A9_INTC_CURRENT		0x81cU	/* current unmasked source */
#define A9_INTC_RAISE		0x900U	/* group 1 raise, 0102c7c6 */
#define A9_INTC_ACK		0x980U	/* group 1 pre-handler ack/clear, 0102c6f2 */
#define A9_INTC_DISABLE		0xa00U	/* group 1 disable, store at 0102c9ce */
#define A9_INTC_ENABLE		0xa80U	/* group 1 enable; reads the same mask */
#define A9_INTC_G7_DISABLE	0xc020U	/* group 7 disable, 2 after sleep */
#define A9_INTC_SPI0_SCM	0x00100000U	/* IRQ 0x10014 */
#define A9_INTC_ALL_ONES	0xffffffffU
#define A9_INTC_REGS		10

static const u32 a9_intc_offsets[A9_INTC_REGS] = {
	A9_INTC_RESET, A9_INTC_CURRENT, A9_INTC_RAISE, A9_INTC_ACK,
	A9_INTC_DISABLE, A9_INTC_DISABLE + 4, A9_INTC_DISABLE + 8,
	A9_INTC_ENABLE, A9_INTC_G7_DISABLE, A9_INTC_G7_DISABLE + 4,
};

/* Snapshot indices into a9_intc_offsets. */
enum {
	A9_INTC_I_RESET, A9_INTC_I_CURRENT, A9_INTC_I_RAISE, A9_INTC_I_ACK,
	A9_INTC_I_MASK0, A9_INTC_I_MASK1, A9_INTC_I_MASK2, A9_INTC_I_ENABLE0,
	A9_INTC_I_G7_0, A9_INTC_I_G7_1,
};

/* When the AP snapshots the INTC words. */
enum {
	A9_INTC_AT_PRE_DISABLE, A9_INTC_AT_POST_DISABLE, A9_INTC_AT_PRE_ACK,
	A9_INTC_AT_POST_ACK, A9_INTC_AT_N,
};

static inline const char *a9_intc_at_name(unsigned int at)
{
	static const char * const names[] = {
		"pre-disable", "post-disable", "pre-ack", "post-ack",
	};

	return at < A9_INTC_AT_N ? names[at] : "unknown";
}

/* "all-ones" if +0x81c reads ffffffff, which a live INTC cannot report. */
static inline const char *a9_intc_view(const u32 *state)
{
	return state[A9_INTC_I_CURRENT] == A9_INTC_ALL_ONES ? "all-ones" : "intc";
}
/*
 * Read only, never written: the SCM state (also per idle-poll slice) and,
 * around the acknowledge, the C3 interval pair.
 */
#define A9_SPI0_SCM_LO		0x00cU
#define A9_SPI0_SCM_HI		0x010U
#define A9_SPI0_CH3_IVAL	0x290U
#define A9_SPI0_CH3_CURSOR	0x294U
#define A9_SPI0_SCM_REGS	4

static const u32 a9_spi0_scm_offsets[A9_SPI0_SCM_REGS] = {
	A9_SPI0_SCM_LO, A9_SPI0_SCM_HI, A9_SPI0_CH3_IVAL, A9_SPI0_CH3_CURSOR,
};

struct a9_aop_intc {
	u32 state[A9_INTC_AT_N][A9_INTC_REGS];	/* around each store */
	unsigned int states;			/* bit n: state[n] was read */
	u32 scm[2][A9_SPI0_SCM_REGS];		/* before / after the ack */
	unsigned int scm_at;			/* scm[] row the guarded read fills */
	bool disabled, acked;
	bool reset_done;
	u32 reset_asserted, reset_after;
	const char *failed;			/* distinct refusal reason, or NULL */
};

/* Read-only milestones. SCM/interval values are observations, never zero gates. */
struct a9_aop_spi0_trace {
	u32 control, clock, scm[A9_SPI0_SCM_REGS], ch3[A9_SPI0_CH3_REGS];
};

/*
 * First failed readback after the teardown, or NULL if all hold. C3C bit 5
 * is a pulse: at 19:50 the write of 0x00200029 read back 0x00200009 while
 * +0x8d0 bit 6 went 1 -> 0, so the acknowledge is judged by bit 6 alone and
 * the C3C readback is only logged.
 */
static inline const char *a9_spi0_ch3_verify(const u32 *after)
{
	if (after[A9_CH3_I_C3] & A9_SPI0_CH3_RUN)
		return "failed-ch3-teardown-run";
	if (after[A9_CH3_I_C3] & A9_SPI0_CH3_IRQ0)
		return "failed-ch3-teardown-class0";
	if (after[A9_CH3_I_C3] & A9_SPI0_CH3_IRQ1)
		return "failed-ch3-teardown-class1";
	if (!(after[A9_CH3_I_C3] & A9_SPI0_CH3_IDLE))
		return "failed-ch3-teardown-idle";
	if (after[A9_CH3_I_DESC])
		return "failed-ch3-teardown-desc";
	if (after[A9_CH3_I_8D0] & A9_SPI0_CH3_PENDING)
		return "failed-ch3-teardown-pending";
	return NULL;
}

/*
 * Has firmware run since the last SoC reset? auto-boot alone cannot say:
 * the outgoing aop_quiesce=1 halt clears it, exactly like a DFU cold start.
 * The previous kernel's reserved-RAM shutdown transcript says whether its
 * firmware ran. That DRAM can outlive a DFU; wrapper +0x4020 outbox enable
 * cannot: set after reset (12/12 DFU cold snapshots), cleared by the first
 * store of every Linux stop. No SPI0 access is needed to decide.
 */
enum a9_spi0_prev_state {
	A9_SPI0_PREV_MISSING,	/* no transcript magic, or none reserved */
	A9_SPI0_PREV_INVALID,	/* magic present, header/checksum rejected */
	A9_SPI0_PREV_VALID,
};

struct a9_spi0_prev {
	enum a9_spi0_prev_state state;
	bool hello;	/* "SHUTDOWN begin ... hello=1": its firmware ran */
	bool halted;	/* "QUIESCE step=complete ... halted=1" + steps bit */
	bool carried;	/* "SPI0-CARRY retained=1": ran earlier in the chain */
	u32 steps;	/* "QUIESCE step=complete ... steps=0x..." */
};

enum a9_spi0_reason {
	/* Run the reset. */
	A9_SPI0_LIVE_AUTO_BOOT,	/* auto-boot 1 before the stop */
	A9_SPI0_PREV_HALTED,	/* previous kernel quiesced and halted it */
	A9_SPI0_PREV_LIVE,	/* previous kernel's firmware ran, no halt */
	A9_SPI0_PREV_CARRIED,	/* an earlier kernel in the chain ran it */
	/* Skip: the block is not touched. */
	A9_SPI0_FRESH,		/* no transcript, auto-boot 0: DFU cold */
	A9_SPI0_PREV_BAD,	/* transcript present but corrupt */
	A9_SPI0_PREV_NO_RUN,	/* transcript: nothing ran in the chain */
	A9_SPI0_STALE,		/* run evidence, wrapper at reset state */
};

static inline const char *a9_spi0_reason_name(enum a9_spi0_reason reason)
{
	static const char * const names[] = {
		"live-auto-boot", "prev-quiesce-halted", "prev-live", "prev-carried",
		"fresh-instance", "prev-transcript-invalid", "prev-no-run",
		"stale-transcript",
	};

	return reason <= A9_SPI0_STALE ? names[reason] : "unknown";
}

static inline const char *a9_spi0_prev_name(enum a9_spi0_prev_state state)
{
	return state == A9_SPI0_PREV_VALID ? "valid" :
	       state == A9_SPI0_PREV_INVALID ? "invalid" : "missing";
}

static inline bool a9_spi0_reason_runs(enum a9_spi0_reason reason)
{
	return reason <= A9_SPI0_PREV_CARRIED;
}

/* Token search inside one transcript line, which holds no NUL. */
static inline bool a9_spi0_line_has(const char *line, size_t length,
				    const char *token, size_t n,
				    const char **at)
{
	size_t i;

	for (i = 0; n <= length && i <= length - n; i++) {
		if (!memcmp(line + i, token, n)) {
			*at = line + i + n;
			return true;
		}
	}
	return false;
}

#define a9_spi0_has(line, length, token, at) \
	a9_spi0_line_has(line, length, token, sizeof(token) - 1, at)

static inline bool a9_spi0_line_starts(const char *line, size_t length,
				       const char *prefix, size_t n)
{
	return length >= n && !memcmp(line, prefix, n);
}

#define a9_spi0_starts(line, length, prefix) \
	a9_spi0_line_starts(line, length, prefix, sizeof(prefix) - 1)

static inline u32 a9_spi0_hex(const char *p, const char *end)
{
	u32 value = 0;

	for (; p < end; p++) {
		if (*p >= '0' && *p <= '9')
			value = value << 4 | (u32)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			value = value << 4 | (u32)(*p - 'a' + 10);
		else
			break;
	}
	return value;
}

/* Our own checksummed transcript; only whole-line prefixes count. */
static inline void a9_spi0_prev_parse(struct a9_spi0_prev *p,
				      const char *text, size_t length)
{
	const char *end = text + length, *line, *next, *at;
	size_t n;

	for (line = text; line < end; line = next < end ? next + 1 : end) {
		next = memchr(line, '\n', end - line);
		if (!next)
			next = end;
		n = next - line;
		if (a9_spi0_starts(line, n, "SHUTDOWN begin ") &&
		    a9_spi0_has(line, n, " hello=1 ", &at))
			p->hello = true;
		if (a9_spi0_starts(line, n, "QUIESCE step=complete ")) {
			if (a9_spi0_has(line, n, " steps=0x", &at))
				p->steps = a9_spi0_hex(at, next);
			p->halted = a9_spi0_has(line, n, " halted=1 ", &at) &&
				    (p->steps & (1U << A9_HO_STEP_HALTED));
		}
		if (a9_spi0_starts(line, n, "SPI0-CARRY retained=1 "))
			p->carried = true;
	}
}

/*
 * Skips unless there is positive evidence: running the reset on a fresh
 * block would be the first AP access to SPI0 with nothing retained to
 * clear (and ps_spi0 may gate it), while a skip on retained state only
 * repeats the known, non-hanging accel STARTEP crash.
 */
static inline enum a9_spi0_reason a9_spi0_decide(bool auto_boot, bool outbox_enabled,
						 const struct a9_spi0_prev *p)
{
	if (auto_boot)
		return A9_SPI0_LIVE_AUTO_BOOT;
	if (p->state == A9_SPI0_PREV_MISSING)
		return A9_SPI0_FRESH;
	if (p->state != A9_SPI0_PREV_VALID)
		return A9_SPI0_PREV_BAD;
	if (!p->hello && !p->carried)
		return A9_SPI0_PREV_NO_RUN;
	if (outbox_enabled)
		return A9_SPI0_STALE;
	if (p->hello)
		return p->halted ? A9_SPI0_PREV_HALTED : A9_SPI0_PREV_LIVE;
	return A9_SPI0_PREV_CARRIED;
}

struct a9_aop_spi0 {
	void __iomem *base;		/* mapped only with aop_spi0_reset=1 */
	u32 before[A9_SPI0_REGS], after[A9_SPI0_REGS];
	u32 saved;			/* control word before the reset */
	u32 asserted;			/* saved & ~0x101 | 0x100 */
	u32 polled;			/* control word when the polls ended */
	u32 prerun;			/* +0x8d0 just before RUN */
	u32 pmgr_before, pmgr_after;	/* AP PMGR ps_spi0 raw, read-only */
	int pmgr_before_errno, pmgr_after_errno;
	int error;
	bool ran, reasserted;
	const char *skipped;
	struct a9_spi0_prev prev;	/* parsed at probe from the transcript */
	u32 auto_boot, outbox;		/* boot snapshot, before the stop */
	enum a9_spi0_reason reason;
	bool decided;
	struct a9_aop_spi0_ch3 ch3;	/* after a clean reset only */
	struct a9_aop_intc intc;	/* IRQ 0x10014 around the reset */
	struct a9_aop_spi0_trace trace;	/* latest read-only milestone */
};

/* 0102f2f4: bits 8 and 0 clear; the reset write adds bit 8. */
static inline u32 a9_spi0_base_word(u32 saved)
{
	return saved & ~(A9_SPI0_RESET | A9_SPI0_ENABLE);
}

#endif
