/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Production aop_handoff=sleep code: shutdown quiesce without halt, record
 * writer/validator, refusals, consumed-once, wake by HELLO or in-place
 * resume and two warm hops, on the real protocol callbacks, plus the
 * aop_wake_wrapper=1 resume-wrapper replay before 0x220. The AOP peer, its
 * sleep and every reply are MODELED; no hardware wake follows from this.
 */
#define A9_SHUTDOWN_TEST
#define A9_HANDOFF_TEST
#define main stage3_replay_main
#include "stage3.c"
#undef main
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define SHA256_DIGEST_SIZE 32
#include "handoff-wrapper.h"

#define EBUSY 16
#define ESHUTDOWN 108
#define EPROBE_DEFER 517
#define HZ 1000
#define min(a, b) ((a) < (b) ? (a) : (b))
#define wmb() ((void)0)
#define vmalloc(n) malloc(n)
#define vfree(p) free(p)
#define readl(p) (*(volatile u32 *)(p))
#undef wait_event_timeout
#define wait_event_timeout(q, cond, t) ({ (void)(q); (void)(t); (cond); })
#define cancel_work_sync(x) ((void)(x))
#define cancel_delayed_work_sync(x) ((void)(x))
#define flush_workqueue(x) ((void)(x))
#define disable_irq(x) ((void)(x))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define devm_kzalloc(dev, size, gfp) ((void)(dev), (void)(gfp), calloc(1, (size)))
#include <stdarg.h>
/* Host stand-in for the kernel's vscnprintf, including %*phN hex dumps. */
static size_t vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	size_t n = 0;

	while (*fmt && n + 1 < size) {
		char spec[16], piece[256];
		size_t len = 0;
		int w;

		if (*fmt != '%') {
			buf[n++] = *fmt++;
			continue;
		}
		if (!strncmp(fmt, "%*phN", 5)) {
			int count = va_arg(args, int);
			const u8 *bytes = va_arg(args, const u8 *);

			for (int i = 0; i < count && n + 3 < size; i++)
				n += snprintf(buf + n, size - n, "%02x", bytes[i]);
			fmt += 5;
			continue;
		}
		spec[len++] = *fmt++;
		while (*fmt && strchr("0123456789.-+ #", *fmt) && len < 8)
			spec[len++] = *fmt++;
		while (*fmt && strchr("hlz", *fmt) && len < 12)
			spec[len++] = *fmt++;
		spec[len++] = *fmt;
		spec[len] = 0;
		switch (*fmt++) {
		case 's':
			w = snprintf(piece, sizeof(piece), spec, va_arg(args, const char *));
			break;
		case 'd': case 'i':
			w = strstr(spec, "ll") ? snprintf(piece, sizeof(piece), spec, va_arg(args, long long)) :
			    strchr(spec, 'z') ? snprintf(piece, sizeof(piece), spec, va_arg(args, ssize_t)) :
			    snprintf(piece, sizeof(piece), spec, va_arg(args, int));
			break;
		case 'u': case 'x': case 'X':
			w = strstr(spec, "ll") ? snprintf(piece, sizeof(piece), spec, va_arg(args, unsigned long long)) :
			    strchr(spec, 'z') ? snprintf(piece, sizeof(piece), spec, va_arg(args, size_t)) :
			    snprintf(piece, sizeof(piece), spec, va_arg(args, unsigned int));
			break;
		case '%':
			w = snprintf(piece, sizeof(piece), "%%");
			break;
		default:
			abort(); /* a kernel extension the transcript does not use */
		}
		for (int i = 0; i < w && n + 1 < size; i++)
			buf[n++] = piece[i];
	}
	buf[n] = 0;
	return n;
}

/* Evaluate every dev_info argument (production reads status registers in
 * log lines), discard the text.
 */
#undef dev_info
static void test_dev_info(const void *dev, const char *fmt, ...)
{
	(void)dev; (void)fmt;
}
#define dev_info(dev, ...) test_dev_info(dev, __VA_ARGS__)

struct platform_device { struct a9_aop *aop; };
struct apple_pmgr_observation { int state; };
static struct a9_aop *platform_get_drvdata(struct platform_device *p) { return p->aop; }
static char cmdline[] = "earlycon linux_a9.entry=aop-handoff-accel-0123456789 aop_handoff=sleep";
static char *saved_command_line = cmdline;
static u32 reg_cpu, reg_inbox = 0x20001, reg_outbox = 0x20001, reg_auto_boot = 1;
/* AP-view notification masks as captured on the slept AOP: all masked. */
static u32 reg_mask = 0x1111, reg_mirror = 0x1111;
static bool wake_wrapper, peer_needs_wrapper;
static unsigned int nwrites, write_fault_at;
static struct { u32 offset, value; unsigned int ntx; } wr_log[16];
static u8 sram[A9_HO_SRAM_SPAN];
static int pmgr_error;
static bool ramoops = true;
static unsigned int checkpoints;

static u32 aop_rd(struct a9_aop *aop, u32 offset)
{
	(void)aop;
	if (offset == AOP_CPU_CONTROL)
		return reg_cpu;
	if (offset == AOP_INBOX_CONTROL)
		return reg_inbox;
	if (offset == AOP_IRQ_MASK_SET)
		return reg_mask;
	if (offset == AOP_IRQ_MIRROR_SET)
		return reg_mirror;
	assert(offset == AOP_OUTBOX_CONTROL); /* status only, never a FIFO pop */
	return reg_outbox;
}
/* Wrapper stores: only the resume masks and outbox enable, never RUN,
 * auto-boot or the NMI pair; each records how many words were sent before.
 */
static int aop_wr(struct a9_aop *aop, u32 offset, u32 value)
{
	(void)aop;
	assert(wake_wrapper && nwrites < ARRAY_SIZE(wr_log));
	assert(offset != AOP_CPU_CONTROL && offset != 0xc004 && offset != 0xc024);
	wr_log[nwrites].offset = offset;
	wr_log[nwrites].value = value;
	wr_log[nwrites].ntx = ntx;
	if (++nwrites == write_fault_at)
		return -EIO;
	if (offset == AOP_IRQ_MASK_SET)
		reg_mask |= value;
	else if (offset == AOP_IRQ_MASK_CLEAR)
		reg_mask &= ~value;
	else if (offset == AOP_IRQ_MIRROR_SET)
		reg_mirror |= value;
	else if (offset == AOP_IRQ_MIRROR_CLEAR)
		reg_mirror &= ~value;
	else if (offset == AOP_OUTBOX_CONTROL)
		reg_outbox = value;
	else
		abort();
	return 0;
}
static int apple_pmgr_observe(void **nodes, unsigned int count,
			      struct apple_pmgr_observation *states,
			      void (*read)(void *), void *data)
{
	(void)nodes; (void)states;
	assert(count == AOP_DOMAINS);
	if (pmgr_error)
		return pmgr_error;
	read(data);
	return 0;
}
static int a9_aop_fromio32(void *base, size_t aperture, u8 *dst, size_t offset, size_t size)
{
	(void)base;
	assert(aperture == 0x9b000 && offset + size <= sizeof(sram));
	memcpy(dst, sram + offset, size);
	return 0;
}
/* Content-sensitive stand-in for the kernel's SHA-256 library. */
static void sha256(const void *data, size_t size, u8 *out)
{
	const u8 *p = data;
	u64 h[4] = { 0xcbf29ce484222325ULL, 0x84222325cbf29ce4ULL, 1, (u64)size };

	for (size_t i = 0; i < size; i++)
		h[i & 3] = (h[i & 3] ^ p[i]) * 0x100000001b3ULL;
	memcpy(out, h, sizeof(h));
}
static void msleep(unsigned int ms) { clock_ns += ms * NSEC_PER_MSEC; }
static bool a9_aop_ramoops_ready(void) { return ramoops; }
static int a9_aop_cold_checkpoint(struct a9_aop *aop, const char *step)
{
	(void)aop;
	assert(!strcmp(step, "handoff-before-wake"));
	checkpoints++;
	return 0;
}
static int a9_aop_wait_firmware(struct a9_aop *aop, const char *name, const struct firmware **fw)
{
	assert(!strcmp(name, AOP_CAL_FW));
	return request_firmware_direct(fw, name, aop->dev);
}
/* Handoff never halts or drains; only the aop_quiesce=1 transcript case may. */
static bool allow_halt;
static unsigned int halts, drains;
static int a9_aop_force_stop(struct a9_aop *aop)
{
	assert(allow_halt && !handoff && quiesce && aop->shutdown_quiesced);
	halts++; aop->force_halted = true;
	return 0;
}
static int a9_aop_force_drain(struct a9_aop *aop)
{
	assert(allow_halt && aop->force_halted);
	drains++; aop->force_drained = true;
	return 0;
}
/* aop_spi0_reset=1 adds the SPI0-CARRY transcript line. */
static bool spi0_reset;
#include "slog-functions.h"
#include "handoff-functions.h"
#include "shutdown-functions.h"

enum wake_mode { WAKE_SILENT, WAKE_HELLO, WAKE_RESUME, WAKE_CRASH, WAKE_BADBUF };
static enum wake_mode wake_mode;
static bool shutting_down, peer_asleep, kernel_waking;
static unsigned int afk_shutdowns, disables, power_step, wakes, drop_sleep_ack;
/* A just-woken firmware may be slow: delayed first ping ACK and AP-on ACK. */
static bool slow_resume;

static void shutdown_property(struct a9_aop *aop, u8 ep)
{
	struct a9_ring *ring = &a9_aop_find_ep(aop, ep)->rings[RING_TX];
	struct a9_ring_entry entry;
	struct a9_spu_subpacket sub;
	u8 packet[256], result[4] = {};
	u32 cursor;
	size_t at = 16;

	assert(!a9_ring_read(ring, a9_ring_wptr(ring), packet, sizeof(packet), &entry, &cursor));
	assert(a9_spu_next(packet, entry.length, &at, &sub) == 1 && sub.subtype == 4);
	assert(get_unaligned_le32(sub.payload + 8) == 0);
	ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
	peer_packet(aop, A9_SPU_REPLY, 0x84, sub.tag, result, sizeof(result));
	enqueue(clock_ns + NSEC_PER_MSEC, ep, a9_afk_word(A9_AFK_RX_NOTIFY));
}

/* Modeled replies to wake220. HELLO mode is iOS's post-RUN-cycle sequence;
 * RESUME follows rtk_power_transition: WFI exit restores 0x21, then ACKs 0x20.
 */
static void wake_reply(void)
{
	u64 at = clock_ns + NSEC_PER_MSEC;

	wakes++;
	if (wake_mode == WAKE_HELLO || wake_mode == WAKE_BADBUF) {
		enqueue(at, 0, 0x00100000000c000cULL);
		enqueue(at + NSEC_PER_MSEC, 0, 0x0080000000000017ULL);
		enqueue(at + 2 * NSEC_PER_MSEC, 0, 0x0088000100073fffULL);
		enqueue(at + 3 * NSEC_PER_MSEC, 0, a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, 0x20));
	} else if (wake_mode == WAKE_RESUME) {
		enqueue(at, 0, a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, 0x20));
	} else if (wake_mode == WAKE_CRASH) {
		enqueue(at, 1, 0x0010000000000000ULL);
	}
}

/* Fresh handshake: the firmware asks for its system buffers again. */
static void system_restart(struct a9_aop *aop, unsigned int ep)
{
	const struct a9_ho_sys *grant = &aop->ho.rec.sys[ep];
	u64 next = clock_ns + NSEC_PER_MSEC, pa = A9_EXP_POOL_PA + grant->offset;

	assert((ep == 1 || ep == 2 || ep == 4) && grant->size);
	if (ep == 2) {
		enqueue(next, 2, a9_mgmt_word(8, (u64)grant->message_size << 24 | grant->entries));
		enqueue(next, 2, a9_mgmt_word(1, (u64)(grant->size >> 12) << 44));
		return;
	}
	if (wake_mode == WAKE_BADBUF && ep == 1)
		pa += A9_SYS_PAGE;
	enqueue(next, ep, a9_mgmt_word(1, (u64)(grant->size >> 12) << 44 | pa));
}

static bool shutdown_peer_tx(struct a9_aop *aop, u8 ep, u64 payload)
{
	u64 next = clock_ns + NSEC_PER_MSEC;
	u8 type = ep ? a9_afk_type(payload) : a9_mgmt_type(payload);

	if (peer_asleep) {
		/* Exactly one wake word, from the next kernel, reaches the sleeper. */
		assert(!ep && payload == A9_HO_IOP_WAKE && kernel_waking && !shutting_down);
		/* 13:27 hypothesis: with bits 0/12 masked in either AP view the
		 * word stays queued and the WFI never ends.
		 */
		if (peer_needs_wrapper &&
		    ((reg_mask | reg_mirror) & (AOP_IRQ_BIT0 | AOP_IRQ_BIT12)))
			return true;
		peer_asleep = false;
		wake_reply();
		return true;
	}
	if (shutting_down) {
		if (ep == 0x21 && type == A9_AFK_DOORBELL) {
			assert(!afk_shutdowns && !power_step);
			shutdown_property(aop, ep);
			return true;
		}
		if (ep >= 0x20 && type == A9_AFK_SHUTDOWN) {
			assert(!power_step && !disables);
			afk_shutdowns++;
			enqueue(next, ep, a9_afk_word(A9_AFK_SHUTDOWN_ACK));
			return true;
		}
		if (!ep && type == A9_MGMT_STARTEP && (u32)payload == 1) {
			assert(a9_aop_apps_stopped(aop) && !power_step);
			disables++;
			return true;
		}
		if (!ep && type == 0xb) {
			assert(payload == a9_sys_ap_quiesce() && !power_step);
			power_step = 1;
			enqueue(next, 0, a9_mgmt_word(0xb, 0x10));
			return true;
		}
		if (!ep && type == 6) {
			assert(payload == a9_sys_iop_sleep() && power_step == 1);
			power_step = 2;
			if (!drop_sleep_ack) {
				enqueue(next, 0, a9_mgmt_word(7, 0x201));
				peer_asleep = true;
			}
			return true;
		}
		assert(!power_step);
		return false;
	}
	if (kernel_waking) {
		if (!ep && (type == A9_MGMT_HELLO_REPLY || type == A9_MGMT_EPMAP ||
			    type == A9_MGMT_AP_POWER))
			return true;
		if (!ep && type == A9_MGMT_STARTEP && ((payload >> 32) & 0xff) < 0x20) {
			system_restart(aop, (payload >> 32) & 0xff);
			return true;
		}
		if (!ep && payload == A9_HO_IOP_WAKE)
			return true; /* rtk_iop_set_target: unchanged target, no ACK */
		if (!ep && payload == A9_HO_AP_ON) {
			enqueue(slow_resume ? clock_ns + 1500 * NSEC_PER_MSEC : next, 0,
				a9_mgmt_word(0xb, 0x20));
			return true;
		}
		if (!ep && type == A9_MGMT_PING && slow_resume) {
			slow_resume = false;
			enqueue(clock_ns + 2 * NSEC_PER_SEC, 0, a9_mgmt_word(A9_MGMT_PING_ACK, (u32)payload));
			return true;
		}
	}
	return false;
}

static void peer_reset(void)
{
	ntx = head = tail = property_count = 0;
	configure_milestone = 0;
	enabled_apps = peer_init = 0;
	firmware_cal_state = firmware_report_interval = 0;
	memset(&iio, 0, sizeof(iio));
	lock_waiter = NULL; race_after_rx = replay_ping_race = false;
	unsupported_arot = true; unsupported_property = 3; expected_cal_state = 2;
	afk_shutdowns = disables = power_step = 0;
}

static void map_tail(struct a9_aop *aop)
{
	aop->ho.slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
	aop->ho.log = (struct a9_sl_header *)((u8 *)aop->ho.slot + A9_HO_LOG_OFFSET);
	aop->ho.log_text = (char *)(aop->ho.log + 1);
}

/* What that probe parsed for the aop_spi0_reset=1 decision. */
static struct a9_spi0_prev replayed_prev;

/* The next probe's replay: returns the previous transcript, once. */
static char *replay(void)
{
	struct a9_aop aop = { .pool = pool };
	char *text;

	map_tail(&aop);
	a9_aop_slog_replay(&aop);
	assert(aop.ho.prev_log && aop.ho.prev_len && aop.ho.log->magic == 0);
	replayed_prev = aop.spi0.prev;
	text = strndup(aop.ho.prev_log, aop.ho.prev_len);
	free(aop.ho.prev_log);
	aop.ho.prev_log = NULL;
	/* Replayed exactly once: a second probe finds nothing. */
	a9_aop_slog_replay(&aop);
	assert(!aop.ho.prev_log);
	return text;
}

static void in_order(const char *text, const char *const *tokens, unsigned int count)
{
	const char *at = text;

	for (unsigned int i = 0; i < count; i++) {
		const char *found = strstr(at, tokens[i]);

		if (!found)
			fprintf(stderr, "transcript missing/out-of-order: %s\n", tokens[i]);
		assert(found);
		at = found + strlen(tokens[i]);
	}
}

/* All 17 sources finish their serialized INIT before the kexec. */
static void settle(struct a9_aop *aop)
{
	assert(a9_aop_wait(aop, peer_init == 17, AOP_SERVICE_MS));
}

static struct a9_aop next_kernel(u8 id, enum wake_mode mode)
{
	struct a9_aop aop = { .experiment_stage = 3, .cold_run = 2, .accel = -1,
		.phase = PHASE_PROBE, .pool = pool, .pool_pa = A9_EXP_POOL_PA,
		.pool_size = sizeof(pool) - A9_HO_PAGE, .region_pa = 0x210e00000ULL,
		.region_size = 0x9b000, .autoboot = &reg_auto_boot, .poll_ms = 10 };

	/* Probe: the tail page leaves the allocator before ext or grants. */
	map_tail(&aop);
	aop.ho.seen = aop.ho.slot->magic == A9_HO_MAGIC;
	memset(aop.ho.writer, id, sizeof(aop.ho.writer));
	peer_reset();
	reg_mask = reg_mirror = 0x1111; nwrites = 0;
	peer_enabled = true; kernel_waking = true; shutting_down = false;
	wake_mode = mode;
	return aop;
}

static int shutdown(struct a9_aop *aop)
{
	struct platform_device pdev = { aop };
	size_t used = aop->pool_used;

	kernel_waking = false; shutting_down = true; peer_enabled = true;
	afk_shutdowns = disables = power_step = 0;
	a9_aop_shutdown(&pdev);
	shutting_down = false;
	assert(aop->pool_used == used && !aop->lock);
	assert(!handoff || (!aop->force_halted && !aop->force_drained && !halts && !drains));
	return aop->ho.slot->state;
}

static void check_valid(const struct a9_aop *aop, u32 generation)
{
	const struct a9_ho_record *r = aop->ho.slot;
	u8 digest[A9_HO_DIGEST];
	const struct a9_ho_expect x = { 0x210e00000ULL, A9_EXP_POOL_PA, 0x9b000,
					sizeof(pool) - A9_HO_PAGE };

	sha256(r, A9_HO_DIGESTED, digest);
	assert(a9_ho_check(r, digest, &x) == A9_HO_OK);
	assert(r->state == A9_HO_VALID && r->generation == generation && !r->quiesce_errno);
	assert(!strcmp(r->entry, "aop-handoff-accel-0123456789"));
	assert(r->pool_used == aop->pool_used && r->nafk == 17 && r->ap_ack == 0x10 && r->iop_ack == 0x201);
	assert(r->sys[1].size == 0x1000 && r->sys[2].size == 0x1000 && r->sys[4].size == 0x4000 && !r->sys[3].size);
	assert(r->sys[2].entries == 16 && r->sys[2].message_size == 80);
	assert(!memcmp(r->writer, aop->ho.writer, sizeof(r->writer)));
	assert(r->steps == A9_HO_STEPS_HANDOFF && !r->failed_errno);
	assert(afk_shutdowns == 17 && disables == 17 && power_step == 2);
}

static void stream_more(struct a9_aop *aop)
{
	u64 before = aop->reports;

	for (unsigned int i = 0; i < 3; i++) {
		peer_report(aop);
		mutex_lock(&aop->lock); receive(aop, 0x2185000000000000ULL); mutex_unlock(&aop->lock);
	}
	assert(aop->reports == before + 3 && iio.valid);
}

static void streaming(struct a9_aop *aop)
{
	assert(aop->phase == PHASE_STREAMING && iio.valid && aop->iio == &iio);
	assert(aop->cal_sent == 3 && aop->cal_unsupported == 1 && !aop->arot_sent);
	assert(aop->cal_state == 2 && firmware_cal_state == 2 && aop->rate_set);
	stream_more(aop);
}

static void reseal(struct a9_ho_record *r)
{
	sha256(r, A9_HO_DIGESTED, r->digest);
}

/* Refused before consumption: no TX, no RAM write, no state change. */
static void refuse(const struct a9_ho_record *good, void (*mutate)(struct a9_ho_record *),
		   enum a9_ho_reason expected)
{
	struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
	struct a9_ho_record saved;
	struct a9_aop aop;
	unsigned int before = wakes;

	memcpy(slot, good, sizeof(*slot));
	if (mutate)
		mutate(slot);
	memcpy(&saved, slot, sizeof(saved));
	aop = next_kernel(9, WAKE_HELLO);
	peer_asleep = true;
	assert(!a9_aop_handoff_wake(&aop));
	if (aop.ho.reason != expected)
		fprintf(stderr, "refusal: expected %s got %s\n", a9_ho_reason_names[expected],
			a9_ho_reason_names[aop.ho.reason]);
	assert(aop.ho.reason == expected && aop.phase == PHASE_REFUSED && !aop.ho.adopt);
	assert(!ntx && !nwrites && wakes == before && !memcmp(slot, &saved, sizeof(saved)));
	peer_asleep = false;
}

static void bad_digest(struct a9_ho_record *r) { r->digest[3] ^= 1; }
static void bad_magic(struct a9_ho_record *r) { r->magic = 0; }
static void bad_version(struct a9_ho_record *r) { r->version = 2; reseal(r); }
static void failed(struct a9_ho_record *r) { r->state = A9_HO_FAILED; r->quiesce_errno = 110; reseal(r); }
static void consumed(struct a9_ho_record *r) { r->state = A9_HO_CONSUMED; reseal(r); }
static void overfull(struct a9_ho_record *r) { r->pool_used = r->pool_size + 64; reseal(r); }
static void overlap(struct a9_ho_record *r) { r->afk[1].offset = r->afk[0].offset; reseal(r); }
static void misaligned(struct a9_ho_record *r) { r->sys[2].offset += 8; reseal(r); }
static void moved_pool(struct a9_ho_record *r) { r->pool_pa += A9_HO_PAGE; reseal(r); }
static void bad_ack(struct a9_ho_record *r) { r->iop_ack = 0x20; reseal(r); }
static void unterminated(struct a9_ho_record *r) { memset(r->entry, 'x', sizeof(r->entry)); reseal(r); }
static void no_generation(struct a9_ho_record *r) { r->generation = 0; reseal(r); }
static void busy_writer(struct a9_ho_record *r) { r->outbox_control &= ~A9_HO_MBOX_EMPTY; reseal(r); }

/* Consumed, then the wake itself fails: one wake word, no retry or reload. */
static struct a9_aop wake_failure(const struct a9_ho_record *good, enum wake_mode mode,
				  const char *result)
{
	struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
	struct a9_aop aop;

	memcpy(slot, good, sizeof(*slot));
	aop = next_kernel(7, mode);
	peer_asleep = true;
	assert(!a9_aop_handoff_wake(&aop));
	if (!aop.result || strcmp(aop.result, result))
		fprintf(stderr, "wake failure mode=%d: %s\n", mode, aop.result);
	assert(aop.phase == PHASE_FAILED && !strcmp(aop.result, result) && aop.ho.adopt);
	assert(tx[0] == A9_HO_IOP_WAKE && slot->state == A9_HO_CONSUMED);
	/* Only the HELLO path's system bring-up repeats the (no-op) on word. */
	for (unsigned int i = 1; i < ntx && mode != WAKE_BADBUF; i++)
		assert(tx[i] != A9_HO_IOP_WAKE);
	/* A consumed-but-failed boot certifies failure for the next kernel. */
	assert(shutdown(&aop) == A9_HO_FAILED && slot->quiesce_errno);
	peer_asleep = false;
	return aop;
}

int main(int argc, char **argv)
{
	struct a9_ho_record good;
	struct a9_aop k1, k2, k3, again;
	u64 used;

	assert(argc == 2 || argc == 3); load_calibration(argv[1]);
	for (size_t i = 0; i < sizeof(sram); i++)
		sram[i] = (u8)(i * 7 + 3);
	quiesce = false; handoff = true;

	/* Cold first start, as the deferred trigger would load it. */
	k1 = replay_system(2);
	k1.pool_size = sizeof(pool) - A9_HO_PAGE;
	map_tail(&k1);
	k1.region_pa = 0x210e00000ULL; k1.region_size = 0x9b000; k1.autoboot = &reg_auto_boot;
	k1.ext_dma = A9_EXP_POOL_PA; k1.ext_size = 37024;
	sha256(sram, A9_HO_SRAM_SPAN, k1.ho.sram_sha256); k1.ho.sram_valid = true;
	memset(k1.ho.writer, 1, sizeof(k1.ho.writer));
	peer_enabled = true; unsupported_arot = true; unsupported_property = 3;
	continue_boot(&k1);
	streaming(&k1);
	settle(&k1);
	used = k1.pool_used;
	if (used != 0x56000)
		fprintf(stderr, "k1 pool_used=%zx neps=%u phase=%d\n", (size_t)used, k1.neps, k1.phase);
	assert(used == 0x56000);
	assert(shutdown(&k1) == A9_HO_VALID);
	check_valid(&k1, 1);
	assert(peer_asleep && k1.iop_power == 0x201 && !iio.valid);
	{
		static const char *const tokens[] = {
			"SHUTDOWN begin mode=handoff entry=aop-handoff-accel-0123456789",
			"QUIESCE step=begin", "QUIESCE step=rates-zero status=0",
			"QUIESCE step=afk-send ep=0x", "QUIESCE step=afk-ack ep=0x",
			"QUIESCE step=endpoint-disable ep=0x",
			"QUIESCE step=ap-send word=00b0000000000010 status=0",
			"QUIESCE step=ap-ack state=0x10",
			"QUIESCE step=iop-send word=0060000000000201 status=0",
			"QUIESCE step=iop-ack state=0x201", "QUIESCE step=ram-stable",
			"QUIESCE step=mailbox-idle", "status=0 halt=skipped",
			"QUIESCE step=complete status=0 quiesced=1 halted=0 drained=0 handoff=1 steps=0x7ff failed_step=none",
			"\nHANDOFF record state=valid status=0 check=ok generation=1 steps=0x7ff",
			"\nSTAGE3 checkpoint=shutdown-handoff-complete\n",
		};
		char *text = replay();

		in_order(text, tokens, ARRAY_SIZE(tokens));
		assert(!strstr(text, "step=halt") && !strstr(text, "step=drain"));
		if (argc == 3) {
			FILE *out = fopen(argv[2], "w");

			assert(out && fputs(text, out) >= 0 && !fclose(out));
		}
		free(text);
	}
	puts("PASS handoff shutdown: rates/AFK/disables, AP10 then IOP201, stable pool, idle mailbox; VALID gen1 record, no halt/drain, pool retained");

	/* Hop 1: the woken firmware restarts its handshake (fresh HELLO). */
	k2 = next_kernel(2, WAKE_HELLO);
	assert(k2.ho.seen && a9_aop_handoff_wake(&k2));
	assert(tx[0] == A9_HO_IOP_WAKE && wakes == 1 && checkpoints == 1 && !nwrites);
	assert(k2.ho.path == A9_HO_PATH_HELLO && k2.ho.adopt && k2.system_ready);
	assert(k2.ho.sys_adopted == 2 && k2.ho.sys_regranted == 1 && k2.system_grants == 1);
	assert(k2.ho.slot->state == A9_HO_CONSUMED && k2.ho.slot->consumer[0] == 2);
	assert(k2.pool_used == used && k2.ext_dma == A9_EXP_POOL_PA && k2.ho.generation == 1);
	continue_boot(&k2);
	streaming(&k2);
	settle(&k2);
	assert(k2.ho.afk_reused == 17 && !k2.ho.afk_fresh && k2.pool_used == used);
	/* Consumed once: the same record never wakes the AOP a second time. */
	again = next_kernel(8, WAKE_HELLO);
	assert(again.ho.seen && !a9_aop_handoff_wake(&again));
	assert(again.ho.reason == A9_HO_ALREADY_CONSUMED && !ntx && again.phase == PHASE_REFUSED);
	assert(shutdown(&k2) == A9_HO_VALID);
	check_valid(&k2, 2);
	puts("PASS handoff hop1 (modeled HELLO path): wake220 first, fresh HELLO/EPMAP, system adopt x2 without reply + PA0 regrant, AFK regions reused, 3 cal ACKs/1 unsupported/3d=2, rates, IIO; consumed once; gen2 record");

	/* Hop 2: the firmware resumes its RTKit session in place. */
	k3 = next_kernel(3, WAKE_RESUME);
	slow_resume = true;
	if (!a9_aop_handoff_wake(&k3))
		fprintf(stderr, "resume wake: %s\n", k3.result);
	assert(k3.phase != PHASE_FAILED && !slow_resume);
	assert(k3.ho.path == A9_HO_PATH_RESUME && k3.ho.sys_adopted == 3 && !k3.system_grants);
	assert(k3.handshake_complete && k3.neps == 17 && k3.system[2].entries == 16);
	assert(k3.ap_power_ack == 0x20 && k3.iop_ready && tx[0] == A9_HO_IOP_WAKE);
	for (unsigned int i = 0; i < ntx; i++)
		assert(a9_aop_endpoint(tx[i]) || a9_mgmt_type(tx[i]) != A9_MGMT_STARTEP ||
		       ((tx[i] >> 32) & 0xff) >= 0x20); /* no system STARTEP on resume */
	continue_boot(&k3);
	streaming(&k3);
	settle(&k3);
	assert(k3.ho.afk_reused == 17 && k3.pool_used == used);
	assert(shutdown(&k3) == A9_HO_VALID);
	check_valid(&k3, 3);
	memcpy(&good, k3.ho.slot, sizeof(good));
	puts("PASS handoff hop2 (modeled in-place resume): IOP ACK first -> certified session adopted, AP on only (1.5 s ACK, 2 s first ping ACK: watchdog primed at adoption), apps restarted, streaming; gen3 record");

	/* Refusals leave the record, the AOP and the mailbox untouched. */
	refuse(&good, bad_magic, A9_HO_MISSING);
	refuse(&good, bad_digest, A9_HO_DIGEST_BAD);
	refuse(&good, bad_version, A9_HO_HEADER);
	refuse(&good, failed, A9_HO_NOT_CERTIFIED);
	refuse(&good, consumed, A9_HO_ALREADY_CONSUMED);
	refuse(&good, overfull, A9_HO_LAYOUT);
	refuse(&good, overlap, A9_HO_LAYOUT);
	refuse(&good, misaligned, A9_HO_LAYOUT);
	refuse(&good, moved_pool, A9_HO_LAYOUT);
	refuse(&good, unterminated, A9_HO_LAYOUT);
	refuse(&good, no_generation, A9_HO_LAYOUT);
	refuse(&good, bad_ack, A9_HO_ACKS);
	refuse(&good, busy_writer, A9_HO_ACKS);
	pool[0x13000] ^= 1;
	refuse(&good, NULL, A9_HO_POOL_CHANGED);
	pool[0x13000] ^= 1;
	sram[0x2000] ^= 1;
	refuse(&good, NULL, A9_HO_SRAM_CHANGED);
	sram[0x2000] ^= 1;
	reg_auto_boot = 0; /* a DFU boot: SRAM/pool may survive, the instance did not */
	refuse(&good, NULL, A9_HO_WRAPPER);
	reg_auto_boot = 1;
	reg_inbox = 0x00000101;
	refuse(&good, NULL, A9_HO_WRAPPER);
	reg_inbox = 0x20001; reg_outbox = 0x60001;
	refuse(&good, NULL, A9_HO_WRAPPER);
	reg_outbox = 0x20001; pmgr_error = -EPROBE_DEFER;
	refuse(&good, NULL, A9_HO_UNAVAILABLE);
	pmgr_error = 0; ramoops = false;
	refuse(&good, NULL, A9_HO_UNAVAILABLE);
	ramoops = true;
	/* A self-resumed firmware may already have posted its ACK. */
	assert(a9_ho_check_live(&good, good.pool_sha256, good.sram_sha256, 1, 0x20001, 0x1) == A9_HO_OK);
	puts("PASS handoff refusals: missing/digest/version/failed/consumed/layout/generation/ACK/busy-writer, changed pool, changed SRAM, DFU auto-boot 0, busy inbox, faults, PMGR and ramoops unavailable: no TX, record untouched");

	/* Consumed then failed: bounded, one wake word, no reload; FAILED record. */
	wake_failure(&good, WAKE_SILENT, "failed-handoff-no-wake-reply");
	wake_failure(&good, WAKE_CRASH, "failed-rtkit-crashlog");
	wake_failure(&good, WAKE_BADBUF, "failed-handoff-system-buffer");
	puts("PASS handoff wake failures: silent firmware, crashlog first, mismatched system PA; consumed once, no retry/reload, next record FAILED");

	/* aop_wake_wrapper=1 against a peer that needs the resume wrapper. */
	{
		static const u32 wo[] = { 0x4000, 0xc00, 0x4004, 0xc04, 0x4004, 0xc04, 0x4020 };
		static const u32 wv[] = { 0x1111, 0x1111, 1, 1, 0x1000, 0x1000, 0x20001 };
		struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
		struct a9_aop k;

		peer_needs_wrapper = true;
		/* Without it: the 13:27 transcript, one word queued and silence. */
		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(10, WAKE_RESUME); peer_asleep = true;
		assert(!a9_aop_handoff_wake(&k) && !strcmp(k.result, "failed-handoff-no-wake-reply"));
		assert(!nwrites && ntx == 1 && tx[0] == A9_HO_IOP_WAKE && slot->state == A9_HO_CONSUMED);
		assert(shutdown(&k) == A9_HO_FAILED);
		peer_asleep = false;
		/* With it: seven stores, all before the one 0x220; resume and stream. */
		wake_wrapper = true;
		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(11, WAKE_RESUME); peer_asleep = true;
		assert(a9_aop_handoff_wake(&k) && k.ho.path == A9_HO_PATH_RESUME);
		assert(k.wake_wrapper_done && !k.wake_wrapper_errno && nwrites == 7);
		for (unsigned int i = 0; i < nwrites; i++)
			assert(wr_log[i].offset == wo[i] && wr_log[i].value == wv[i] && !wr_log[i].ntx);
		assert(tx[0] == A9_HO_IOP_WAKE && reg_mask == 0x110 && reg_mirror == 0x110 && !peer_asleep);
		for (unsigned int i = 1; i < ntx; i++)
			assert(tx[i] != A9_HO_IOP_WAKE);
		continue_boot(&k); streaming(&k); settle(&k);
		assert(shutdown(&k) == A9_HO_VALID);
		check_valid(&k, good.generation + 1);
		/* A failed store: no 0x220 at all, record consumed, next FAILED. */
		for (unsigned int fail = 1; fail <= 7; fail++) {
			memcpy(slot, &good, sizeof(*slot));
			k = next_kernel(12, WAKE_RESUME); peer_asleep = true; write_fault_at = fail;
			assert(!a9_aop_handoff_wake(&k) && !strcmp(k.result, "failed-handoff-wrapper-wake"));
			assert(!ntx && nwrites == fail && !k.wake_wrapper_done && k.wake_wrapper_errno == -EIO);
			assert(slot->state == A9_HO_CONSUMED && shutdown(&k) == A9_HO_FAILED);
			peer_asleep = false;
		}
		write_fault_at = 0; wake_wrapper = false; peer_needs_wrapper = false;
	}
	puts("PASS handoff aop_wake_wrapper (modeled peer that needs bits 0/12 unmasked): silent without it; 4000/c00=1111, 4004/c04=1 then 1000, 4020|=1, all before the single 0x220, no RUN/auto-boot/NMI; resume, stream, VALID record; a failed store sends no wake and certifies FAILED");

	/* Outgoing quiesce failures certify failure and still never halt. */
	{
		struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
		struct a9_aop k;

		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(4, WAKE_RESUME); peer_asleep = true;
		assert(a9_aop_handoff_wake(&k)); continue_boot(&k); streaming(&k);
		drop_sleep_ack = 1;
		assert(shutdown(&k) == A9_HO_FAILED && slot->quiesce_errno == ETIMEDOUT);
		assert(slot->failed_step == A9_HO_STEP_IOP_ACKED && slot->failed_errno == ETIMEDOUT);
		assert(slot->steps == (1U << A9_HO_STEP_IOP_ACKED) - 1);
		{
			static const char *const tokens[] = {
				"QUIESCE step=iop-send", "QUIESCE step=iop-timeout",
				"failed_step=iop-ack errno=110", "HANDOFF record state=failed",
				"HANDOFF failed", "STAGE3 checkpoint=shutdown-handoff-failed",
			};
			char *text = replay();

			in_order(text, tokens, ARRAY_SIZE(tokens));
			free(text);
		}
		drop_sleep_ack = 0;
		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(5, WAKE_RESUME); peer_asleep = true;
		assert(a9_aop_handoff_wake(&k)); continue_boot(&k); streaming(&k);
		reg_outbox = 0x00000001; /* a word left in the outbox */
		assert(shutdown(&k) == A9_HO_FAILED && slot->quiesce_errno == EBUSY);
		reg_outbox = 0x20001;
		struct a9_ho_record failed_record;
		memcpy(&failed_record, slot, sizeof(failed_record));
		refuse(&failed_record, NULL, A9_HO_NOT_CERTIFIED);
	}
	/* aop_quiesce=1 writes the same reserved transcript through halt/drain. */
	{
		struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
		static const char *const tokens[] = {
			"SHUTDOWN begin mode=quiesce entry=aop-handoff-accel-0123456789",
			"QUIESCE step=begin", "QUIESCE step=rates-zero status=0",
			"QUIESCE step=ap-ack state=0x10", "QUIESCE step=iop-ack state=0x201",
			"QUIESCE step=ram-stable", "QUIESCE step=halt status=0",
			"QUIESCE step=drain status=0",
			"QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none",
			"\nSTAGE3 checkpoint=shutdown-quiesce-complete\n",
		};
		struct a9_aop k;
		char *text;

		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(6, WAKE_RESUME); peer_asleep = true;
		assert(a9_aop_handoff_wake(&k)); continue_boot(&k); streaming(&k); settle(&k);
		handoff = false; quiesce = true; allow_halt = true;
		shutdown(&k);
		assert(halts == 1 && drains == 1 && k.force_halted && k.force_drained);
		text = replay();
		in_order(text, tokens, ARRAY_SIZE(tokens));
		assert(!strstr(text, "HANDOFF record") && !strstr(text, "SPI0-CARRY"));
		assert(replayed_prev.state == A9_SPI0_PREV_VALID && replayed_prev.hello &&
		       replayed_prev.halted && replayed_prev.steps == 0x1bff && !replayed_prev.carried);
		if (argc == 3) {
			char path[4096];
			FILE *out;

			snprintf(path, sizeof(path), "%s.quiesce", argv[2]);
			out = fopen(path, "w");
			assert(out && fputs(text, out) >= 0 && !fclose(out));
		}
		free(text);
		handoff = true; quiesce = false; allow_halt = false; halts = drains = 0;
	}
	/*
	 * aop_spi0_reset=1 on the same production quiesce: A streams and is
	 * halted (auto-boot 0, like a DFU cold start), the next probe parses
	 * "firmware ran" from the transcript; B never releases the AOP and
	 * still carries it to C. Every decision binds to +0x4020 enable.
	 */
	{
		struct a9_ho_record *slot = (struct a9_ho_record *)(pool + sizeof(pool) - A9_HO_PAGE);
		static const char *const ran[] = {
			"SHUTDOWN begin mode=quiesce entry=aop-handoff-accel-0123456789", " hello=1 ",
			"\nSPI0-CARRY retained=1 source=hello\n", "QUIESCE step=begin",
			"QUIESCE step=halt status=0", "QUIESCE step=drain status=0",
			"QUIESCE step=complete status=0 quiesced=1 halted=1 drained=1 steps=0x1bff failed_step=none",
		};
		static const char *const idle[] = {
			"SHUTDOWN begin mode=quiesce entry=aop-handoff-accel-0123456789", " hello=0 ",
			"\nSPI0-CARRY retained=1 source=prev-unbound\n",
			"QUIESCE step=skipped reason=no-owned-handshake",
		};
		struct a9_aop k;
		char *text;

		memcpy(slot, &good, sizeof(*slot));
		k = next_kernel(7, WAKE_RESUME); peer_asleep = true;
		assert(a9_aop_handoff_wake(&k)); continue_boot(&k); streaming(&k); settle(&k);
		handoff = false; quiesce = true; allow_halt = true; spi0_reset = true;
		shutdown(&k);
		assert(halts == 1 && drains == 1 && k.force_halted && k.force_drained);
		text = replay();
		in_order(text, ran, ARRAY_SIZE(ran));
		free(text);
		assert(replayed_prev.state == A9_SPI0_PREV_VALID && replayed_prev.hello &&
		       replayed_prev.halted && replayed_prev.steps == 0x1bff && replayed_prev.carried);
		assert(a9_spi0_decide(false, false, &replayed_prev) == A9_SPI0_PREV_HALTED);
		assert(a9_spi0_decide(false, true, &replayed_prev) == A9_SPI0_STALE);
		assert(a9_spi0_decide(true, true, &replayed_prev) == A9_SPI0_LIVE_AUTO_BOOT);
		/* B probes that transcript, never releases the AOP, kexecs again. */
		k = next_kernel(8, WAKE_SILENT);
		k.spi0.prev = replayed_prev;
		shutdown(&k);
		assert(!k.hello && halts == 1 && drains == 1);
		text = replay();
		in_order(text, idle, ARRAY_SIZE(idle));
		free(text);
		assert(replayed_prev.state == A9_SPI0_PREV_VALID && !replayed_prev.hello &&
		       !replayed_prev.halted && !replayed_prev.steps && replayed_prev.carried);
		assert(a9_spi0_decide(false, false, &replayed_prev) == A9_SPI0_PREV_CARRIED);
		assert(a9_spi0_decide(false, true, &replayed_prev) == A9_SPI0_STALE);
		/* A stale carry (DFU in between: decided skip) that released nothing
		 * is not passed on.
		 */
		k = next_kernel(9, WAKE_SILENT);
		k.spi0.prev = replayed_prev;
		k.spi0.decided = true; k.spi0.reason = A9_SPI0_STALE;
		shutdown(&k);
		text = replay();
		assert(strstr(text, "\nSPI0-CARRY retained=0 source=stale-transcript\n"));
		free(text);
		assert(replayed_prev.state == A9_SPI0_PREV_VALID && !replayed_prev.hello &&
		       !replayed_prev.carried);
		assert(a9_spi0_decide(false, false, &replayed_prev) == A9_SPI0_PREV_NO_RUN);
		handoff = true; quiesce = false; allow_halt = false; spi0_reset = false;
		halts = drains = 0;
	}
	puts("PASS aop_spi0_reset retained-state transcript: production quiesce writes SPI0-CARRY, next probe parses hello/halted/steps 0x1bff -> prev-quiesce-halted (stale with +0x4020 enable set); idle hop carries prev-carried; a stale skip carries 0; no line without the opt-in");
	puts("PASS reserved shutdown transcript: handoff and aop_quiesce=1 lines replayed once by the next probe, independent of console loglevel; failure step recorded");
	puts("PASS handoff outgoing failures: missing IOP201 ACK and non-empty outbox write FAILED records; no halt, drain or reload");

	/* Pure helpers: iOS b767f8 buffer semantics and once-only AFK reuse. */
	{
		struct a9_ho_sys g = { 0xc000, 0x1000, 0, 0 };
		struct a9_ho_record r = { .nafk = 2 };
		u8 seen[A9_HO_MAX_AFK] = {};
		u32 offset;

		assert(a9_ho_sys_request(&g, A9_EXP_POOL_PA, A9_EXP_POOL_PA + 0xc000, 0x1000) == A9_HO_SYS_ADOPT);
		assert(a9_ho_sys_request(&g, A9_EXP_POOL_PA, A9_EXP_POOL_PA + 0xd000, 0x1000) == A9_HO_SYS_MISMATCH);
		assert(a9_ho_sys_request(&g, A9_EXP_POOL_PA, A9_EXP_POOL_PA + 0xc000, 0x2000) == A9_HO_SYS_MISMATCH);
		assert(a9_ho_sys_request(&g, A9_EXP_POOL_PA, 0, 0x1000) == A9_HO_SYS_REGRANT);
		assert(a9_ho_sys_request(&g, A9_EXP_POOL_PA, 0, 0x2000) == A9_HO_SYS_MISMATCH);
		assert(a9_ho_sys_request(NULL, A9_EXP_POOL_PA, 0, 0x1000) == A9_HO_SYS_FRESH);
		assert(a9_ho_sys_request(NULL, A9_EXP_POOL_PA, 0x1000, 0x1000) == A9_HO_SYS_MISMATCH);
		r.afk[0] = (struct a9_ho_afk){ 0x21, 0, 1, 0x12000, 0x4000 };
		r.afk[1] = (struct a9_ho_afk){ 0x22, 0, 1, 0x16000, 0x4000 };
		assert(!a9_ho_take_afk(&r, seen, 0x21, 1, 0x4000, &offset) && offset == 0x12000);
		assert(a9_ho_take_afk(&r, seen, 0x21, 1, 0x4000, &offset) == -ENOENT);
		assert(a9_ho_take_afk(&r, seen, 0x22, 2, 0x4000, &offset) == -ENOENT);
		assert(a9_ho_take_afk(&r, seen, 0x22, 1, 0x2000, &offset) == -ENOENT);
	}
	puts("PASS handoff helpers: adopt only the exact recorded PA/size, PA0 regrant within size, AFK region reuse once per (ep, tag, size)");
	if (strcmp(argv[1], "-")) free((void *)calibration.data);
	return 0;
}
