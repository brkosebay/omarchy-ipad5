/* SPDX-License-Identifier: GPL-2.0-only */
/* Production shutdown + actual protocol callbacks; modeled AOP peer only. */
#define A9_SHUTDOWN_TEST
#define main stage3_replay_main
#include "stage3.c"
#undef main

#define SHA256_DIGEST_SIZE 32
#define EBUSY 16
struct platform_device { struct a9_aop *aop; };
static unsigned int failure, afk_sent, disables, power_step, hashes, halts, drains;
static bool shutting_down;
/* The pre-fix burst (reproduce_1735) may have several stops outstanding. */
static bool burst;
static u64 first_stop_ns, last_ack_ns;
/* 11-14 and the burst replays: the firmware handles AFK stops one at a time,
 * fw_stop_ns each (longer than the AP's 1 ms inline poll period). */
static u64 fw_stop_ns, fw_busy_ns;
static struct a9_aop *platform_get_drvdata(struct platform_device *p) { return p->aop; }
#define cancel_work_sync(x) ((void)(x))
#define cancel_delayed_work_sync(x) ((void)(x))
#define flush_workqueue(x) ((void)(x))
#define disable_irq(x) ((void)(x))
static void msleep(unsigned int ms) { clock_ns += ms * NSEC_PER_MSEC; }
static void sha256(const void *p, size_t size, u8 *digest)
{
	assert(p == pool && size && power_step == 2);
	memset(digest, failure == 8 ? ++hashes : 0x5a, SHA256_DIGEST_SIZE);
}
static int a9_aop_force_stop(struct a9_aop *aop)
{
	assert(aop->shutdown_quiesced && power_step == 2 && aop->phase == PHASE_STOPPED);
	halts++;
	if (failure == 9) return -EIO;
	aop->force_halted = true; aop->force_halt_control = 0;
	return 0;
}
static int a9_aop_force_drain(struct a9_aop *aop)
{
	assert(aop->force_halted && halts == 1);
	drains++;
	if (failure == 10) return -ETIMEDOUT;
	aop->force_drained = true;
	return 0;
}
/* aop_handoff=sleep stays off in this replay; tests/handoff.c covers it,
 * including the reserved shutdown transcript. */
/* Arguments are still evaluated and format-checked. */
static __attribute__((format(printf, 2, 3))) void tr_line(struct a9_aop *aop, const char *fmt, ...)
{ (void)aop; (void)fmt; }
#define aop_tr_info(aop, ...) tr_line(aop, __VA_ARGS__)
#define aop_tr_err(aop, ...) tr_line(aop, __VA_ARGS__)
static void a9_aop_slog_begin(struct a9_aop *aop) { (void)aop; }
static int a9_aop_handoff_idle(struct a9_aop *aop) { (void)aop; abort(); }
static int a9_aop_handoff_write(struct a9_aop *aop, int status) { (void)aop; (void)status; abort(); }
#include "shutdown-functions.h"

static bool shutdown_peer_tx(struct a9_aop *aop, u8 ep, u64 payload)
{
	u64 next = clock_ns + NSEC_PER_MSEC;
	u8 type = ep ? a9_afk_type(payload) : a9_mgmt_type(payload);
	if (!shutting_down) return false;
	if (ep == 0x21 && type == A9_AFK_DOORBELL) {
		struct a9_ring *ring = &a9_aop_find_ep(aop, ep)->rings[RING_TX];
		struct a9_ring_entry entry;
		struct a9_spu_subpacket sub;
		u8 packet[256], result[4] = {};
		u32 cursor;
		size_t at = 16;
		static const u32 keys[] = { 0x16, 0x5c, 0 };
		assert(!afk_sent && !power_step);
		assert(!a9_ring_read(ring, a9_ring_wptr(ring), packet, sizeof(packet), &entry, &cursor));
		assert(a9_spu_next(packet, entry.length, &at, &sub) == 1 && sub.subtype == 4);
		assert(property_count >= 8 && property_count <= 10);
		assert(get_unaligned_le32(sub.payload + 4) == keys[property_count - 8]);
		assert(get_unaligned_le32(sub.payload + 8) == 0);
		property_count++;
		ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
		if (failure != 1) {
			peer_packet(aop, A9_SPU_REPLY, 0x84, sub.tag, result, sizeof(result));
			enqueue(next, ep, a9_afk_word(A9_AFK_RX_NOTIFY));
		}
		return true;
	}
	if (ep >= 0x20 && type == A9_AFK_SHUTDOWN) {
		u64 at = next;
		assert(!power_step && !disables);
		if (aop->cold_run == 2) assert(property_count == 11);
		/* iOS order: the previous stop was ACKed before this one is sent. */
		for (unsigned int i = 0; i < aop->neps && !burst; i++)
			assert(aop->eps[i].state != AFK_STOPPING);
		if (!afk_sent++) first_stop_ns = clock_ns;
		if (fw_stop_ns) {
			fw_busy_ns = (fw_busy_ns > clock_ns ? fw_busy_ns : clock_ns) + fw_stop_ns;
			at = fw_busy_ns;
		}
		if (failure == 12 && ep == 0x28) at = clock_ns + 1500 * NSEC_PER_MSEC; /* late */
		if (failure == 14) at = clock_ns + 700 * NSEC_PER_MSEC; /* all slow */
		if ((failure != 2 || ep != 0x21) && (failure != 13 || ep != 0x28)) {
			enqueue(at, ep, a9_afk_word(A9_AFK_SHUTDOWN_ACK));
			last_ack_ns = at;
		}
		return true;
	}
	if (!ep && type == A9_MGMT_STARTEP && (u32)payload == 1) {
		assert(a9_aop_apps_stopped(aop) && !power_step);
		disables++;
		return true;
	}
	if (!ep && type == 0xb) {
		assert(payload == a9_sys_ap_quiesce() && !aop->ap_power_acked);
		assert(!power_step && a9_aop_apps_stopped(aop));
		assert(disables == (aop->cold_run == 2 ? 17U : 0U));
		power_step = 1;
		if (failure != 3)
			enqueue(next, 0, a9_mgmt_word(0xb, failure == 4 ? 0x20 : 0x10));
		return true;
	}
	if (!ep && type == 6) {
		assert(payload == a9_sys_iop_sleep() && power_step == 1);
		assert(aop->ap_power_acked && aop->ap_power_ack == 0x10 && aop->iop_power == ~0U);
		power_step = 2;
		if (failure == 7) { aop->iop_crashed = aop->rx_halt = true; return true; }
		if (failure != 5)
			enqueue(next, 0, a9_mgmt_word(7, failure == 6 ? 0x20 : 0x201));
		return true;
	}
	assert(!power_step); /* No watchdog or application TX after AP power. */
	return false;
}

static unsigned int running_eps(const struct a9_aop *aop)
{
	unsigned int i, count = 0;

	for (i = 0; i < aop->neps; i++)
		count += aop->eps[i].state == AFK_RUNNING;
	return count;
}

static void run_shutdown(unsigned int mode, unsigned int run, bool opt_in)
{
	struct a9_aop aop;
	unsigned int running;
	shutting_down = false; quiesce = opt_in;
	aop = replay_system(run); peer_enabled = true;
	if (run == 2) { continue_boot(&aop); assert(aop.phase == PHASE_STREAMING && aop.iio); }
	/* 17:35 had streamed for minutes: all 17 application endpoints running. */
	if (mode >= 11) assert(a9_aop_wait(&aop, running_eps(&aop) == 17, AOP_SERVICE_MS));
	running = running_eps(&aop);
	if (!opt_in)
		for (unsigned int tick = 0; tick < 200; tick++) {
			a9_aop_inline_rx(&aop); usleep_range(500, 1000);
		}
	void *retained = aop.pool;
	size_t used = aop.pool_used;
	struct platform_device pdev = { &aop };
	failure = mode; afk_sent = disables = power_step = hashes = halts = drains = 0;
	/* 11-14: the 17:35 conditions, an 8-word outbox and a console at
	 * dmesg -n 8 (two ~3 ms lines per send, one per received word). */
	burst = false; first_stop_ns = last_ack_ns = fw_busy_ns = 0; outbox_lost = 0;
	fw_stop_ns = mode >= 11 ? 2 * NSEC_PER_MSEC : 0;
	outbox_depth = mode >= 11 ? 8 : 0;
	send_cost_ns = mode >= 11 ? 6 * NSEC_PER_MSEC : 0;
	rx_cost_ns = mode >= 11 ? 3 * NSEC_PER_MSEC : 0;
	last_drain_ns = clock_ns;
	shutting_down = true;
	a9_aop_shutdown(&pdev);
	if (aop.pool != retained || aop.pool_used != used || aop.lock)
		fprintf(stderr, "shutdown mode=%u run=%u opt=%d used=%zx->%zx lock=%d\n",
			mode, run, opt_in, used, aop.pool_used, aop.lock);
	assert(aop.pool == retained && aop.pool_used == used && !aop.lock);
	assert(aop.stopping && aop.inline_rx);
	if (!opt_in) {
		assert(!power_step && !halts && !drains && !aop.shutdown_quiesced);
		if (mode == 11) {
			assert(running == 17 && afk_sent == 17 && !outbox_lost);
			for (unsigned int i = 0; i < aop.neps; i++)
				assert(aop.eps[i].state == AFK_STOPPED);
		}
	} else if (!mode || mode == 11 || mode == 12) {
		assert(power_step == 2 && halts == 1 && drains == 1);
		assert(aop.shutdown_quiesced && aop.force_halted && aop.force_drained);
		assert(!iio.valid && aop.iop_power == 0x201 && aop.ap_power_ack == 0x10);
		/* Same step bitmap as the 16:27 transcript: steps=0x1bff. */
		assert(aop.ho.steps == 0x1bff && !aop.ho.failed_errno);
		assert(afk_sent == running && !outbox_lost);
		/* The 1.5 s ACK passes the 2 s per-stop bound (the old 1 s
		 * aggregate wait would have failed it). */
		if (mode == 12) assert(last_ack_ns - first_stop_ns > 1500 * NSEC_PER_MSEC);
	} else {
		assert(!aop.force_drained);
		assert(halts == (mode >= 9 && mode <= 10 ? 1U : 0U));
		assert(drains == (mode == 10 ? 1U : 0U));
		assert(!aop.shutdown_quiesced || (mode >= 9 && mode <= 10));
		if (mode == 1 || mode == 2) assert(!power_step);
		if (mode == 3 || mode == 4) assert(power_step == 1);
		/* A missing AFK ACK ends the sequence: no later stop, disable or
		 * power step, within the per-stop / total bounds. */
		if (mode == 2) assert(afk_sent == 2);
		if (mode == 2 || mode == 13 || mode == 14) {
			assert(!disables && !power_step);
			assert(aop.ho.failed_step == A9_HO_STEP_AFK_ACKED &&
			       aop.ho.failed_errno == ETIMEDOUT &&
			       !(aop.ho.steps & (1U << A9_HO_STEP_AFK_SENT)));
		}
		if (mode == 13) {
			assert(afk_sent == 9 && a9_aop_find_ep(&aop, 0x28)->state == AFK_STOPPING);
			assert(a9_aop_find_ep(&aop, 0x29)->state == AFK_RUNNING);
			assert(clock_ns - first_stop_ns < (AOP_QUIESCE_EP_MS + 200) * NSEC_PER_MSEC);
		}
		if (mode == 14) {
			assert(afk_sent >= 14 && afk_sent < 17);
			assert(clock_ns - first_stop_ns < (AOP_QUIESCE_AFK_MS + 50) * NSEC_PER_MSEC);
		}
	}
	outbox_depth = 0; send_cost_ns = rx_cost_ns = fw_stop_ns = 0;
	shutting_down = false;
}

/*
 * Model check of the pre-fix quiesce: all 17 stops back to back, then one
 * 1 s aggregate wait, against the 8-word outbox and a serial firmware.
 * Quiet console (16:27): every ACK arrives. dmesg -n 8 (17:35, two ~3 ms
 * lines per send): the outbox fills during the burst and the transcript is
 * reproduced exactly, 0x20..0x27 stopped and 0x28..0x32 stopping.
 */
static void reproduce_burst(bool slow_console)
{
	struct a9_aop aop;
	unsigned int i;

	shutting_down = false; quiesce = true;
	aop = replay_system(2); peer_enabled = true;
	continue_boot(&aop); assert(aop.phase == PHASE_STREAMING);
	assert(a9_aop_wait(&aop, running_eps(&aop) == 17, AOP_SERVICE_MS));
	failure = 0; afk_sent = disables = power_step = 0; burst = true;
	outbox_depth = 8; outbox_lost = 0; fw_stop_ns = 2 * NSEC_PER_MSEC; fw_busy_ns = 0;
	send_cost_ns = slow_console ? 6 * NSEC_PER_MSEC : 0;
	rx_cost_ns = slow_console ? 3 * NSEC_PER_MSEC : 0;
	shutting_down = true;
	aop.stopping = aop.inline_rx = true; aop.phase = PHASE_STOPPED;
	assert(!a9_aop_set_rate(&aop, 0, 0, 0));
	a9_aop_inline_rx(&aop);
	mutex_lock(&aop.lock);
	for (i = 0; i < aop.neps; i++) {
		if (aop.eps[i].state < AFK_INIT || aop.eps[i].state > AFK_RUNNING)
			continue;
		assert(!a9_aop_send(&aop, aop.eps[i].num, a9_afk_word(A9_AFK_SHUTDOWN)));
		aop.eps[i].state = AFK_STOPPING;
	}
	mutex_unlock(&aop.lock);
	assert(a9_aop_wait(&aop, a9_aop_apps_stopped(&aop), AOP_REPLY_MS / 2) == !slow_console);
	assert(afk_sent == 17 && outbox_lost == (slow_console ? 9U : 0U));
	for (i = 0; i < aop.neps; i++)
		assert(aop.eps[i].state == (!slow_console || aop.eps[i].num <= 0x27 ?
					    AFK_STOPPED : AFK_STOPPING));
	outbox_depth = 0; send_cost_ns = rx_cost_ns = fw_stop_ns = 0;
	burst = shutting_down = false;
}

int main(int argc, char **argv)
{
	assert(argc == 2 && strcmp(argv[1], "-"));
	FILE *file = fopen(argv[1], "rb"); assert(file);
	assert(!fseek(file, 0, SEEK_END)); calibration.size = ftell(file); rewind(file);
	u8 *data = malloc(calibration.size); assert(data);
	assert(fread(data, 1, calibration.size, file) == calibration.size); fclose(file);
	calibration.data = data;
	/* Parse count through the real API, which requires storage. */
	struct a9_cal_entry entries[5];
	cal_count = a9_cal_parse(data, calibration.size, entries, ARRAY_SIZE(entries));
	assert(cal_count == 4);
	for (unsigned int cycle = 0; cycle < 2; cycle++) run_shutdown(0, 2, true);
	run_shutdown(0, 1, true);
	run_shutdown(0, 2, false);
	for (unsigned int mode = 1; mode <= 10; mode++) run_shutdown(mode, 2, true);
	puts("PASS production shutdown: two modeled full boots/streams/shutdowns, conservative/default-off; rate/AFK/AP/IOP ACK ordering; missing/wrong ACK, crash, changing RAM, halt/drain failure; pool retained, no halt before power ACKs");
	reproduce_burst(false);
	reproduce_burst(true);
	for (unsigned int mode = 11; mode <= 14; mode++) run_shutdown(mode, 2, true);
	run_shutdown(11, 2, false); /* default-off path: sequential stops too */
	free(data);
	puts("PASS 16:27/17:35 partial-ACK traces: the old 17-stop burst against an 8-word outbox and a serial firmware gets all ACKs with a quiet console and, at dmesg -n 8, exactly 0x20..0x27 stopped / 0x28..0x32 stopping (9 lost); production stops one endpoint at a time and completes (steps=0x1bff, 0 lost) under the 17:35 conditions, takes a 1.5 s ACK, and ends a missing ACK at 0x28 (9 stops, no disable/power step) within 2 s and all-slow ACKs within the 10 s total");
	return 0;
}
