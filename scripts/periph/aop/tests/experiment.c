/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual management, HELLO attempt and grant functions, with a fake clock/FIFO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-experiment.h"
#include "a9-aop-system.h"
#include "a9-aop-handoff.h"

#define ENOMEM 12
#define ETIMEDOUT 110
#define BIT(n) (1ULL << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define AOP_PING_MS 1000
#define AOP_PING_TIMEOUT_MS 5000
#define NSEC_PER_SEC 1000000000ULL
#define div_u64(a,b) ((a) / (b))
#define GFP_KERNEL 0
#define AOP_TRACE 512
#define AOP_MAX_EPS 32
#define AOP_GETBUF_MAX 0x100000
#define AOP_HELLO_MS 5000
#define AOP_EPMAP_MS 2000
#define AOP_IOP_READY_MS 2000
#define NSEC_PER_MSEC 1000000ULL
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define KERN_ERR ""
#define DUMP_PREFIX_OFFSET 0
#define print_hex_dump(...) ((void)0)
#define EIO 5
#define dma_rmb() ((void)0)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define wake_up_all(x) ((void)(x))
#define msecs_to_jiffies(x) (x)
#define time_before(a, b) ((a) < (b))
#define set_bit(n, p) ((p)[(n) / 64] |= 1ULL << ((n) % 64))
#define test_bit(n, p) (!!((p)[(n) / 64] & (1ULL << ((n) % 64))))
#define dma_wmb() ((void)0)

enum phase { PHASE_PROBE, PHASE_WAIT_HELLO, PHASE_WAIT_EPMAP,
	PHASE_WAIT_SERVICE, PHASE_SYSTEM_READY, PHASE_STOPPED, PHASE_FAILED, PHASE_REFUSED };
enum { AFK_OFF, AFK_FAILED };
struct a9_aop_buffer { u16 tag; size_t size; void *va; u64 dma; };
struct a9_aop_system {
	struct a9_aop_buffer buffer;
	bool started;
	u32 entries, message_size;
	u64 words, acks, errors, unknown;
};
struct a9_aop_ep {
	u8 num;
	int state;
	struct a9_aop_buffer bufs[A9_AFK_MAX_BUFFERS];
	unsigned int nbufs;
};
struct a9_aop {
	void *dev, *wq;
	int lock, wait, ping_work;
	bool hello, cold, epmap_done, iop_ready, ap_power_acked;
	bool inline_rx, rx_halt, speculative_hello, handshake_complete;
	u16 hello_min, hello_max;
	unsigned int experiment_stage, epmap_seen, epmap_chunks, neps;
	unsigned int cold_run;
	struct a9_aop_system system[A9_SYS_COUNT];
	bool system_ready, iop_crashed, stopping, shutdown_power_requested;
	u64 system_grants;
	u64 system_words;
	unsigned int device_hellos, speculative_replies;
	const char *experiment_step, *experiment_winner;
	bool mailbox_unmasked;
	u64 ping_sent_ns, ping_ack_ns, ping_acks, unknown_mgmt;
	u32 ping_seq, ap_power_ack, iop_power;
	u64 present[4];
	struct a9_aop_ep eps[AOP_MAX_EPS];
	void *pool;
	u64 pool_pa;
	size_t pool_used, pool_size;
	enum phase phase;
	const char *result;
	int error;
	struct a9_aop_handoff ho;
};
static unsigned long jiffies;
static unsigned int ntx, coherent_allocs, coherent_frees, ping_queued;
static int send_error;
static bool quiesce, handoff;
static u64 tx[64];
static unsigned int experiment_attempted;
static bool cycle;
static unsigned int wake, wake_calls;
static bool auto_map, auto_power, auto_system;
static const char *map_step, *hello_step;
static bool partial_map;
static struct { unsigned long when; u64 word; } events[32];
static unsigned int nevents, next_event;

static unsigned int atomic_cmpxchg(unsigned int *p, unsigned int old, unsigned int value)
{
	unsigned int before = *p;
	if (before == old)
		*p = value;
	return before;
}
static u64 ktime_get_boottime_ns(void) { return jiffies * NSEC_PER_MSEC; }
static void usleep_range(int lo, int hi) { (void)lo; (void)hi; jiffies++; }
static bool a9_aop_dead(struct a9_aop *aop)
{
	return aop->phase == PHASE_FAILED || aop->phase == PHASE_REFUSED;
}
static void a9_aop_finish(struct a9_aop *aop, enum phase phase, const char *why, int error)
{
	if (a9_aop_dead(aop))
		return;
	aop->phase = phase;
	aop->result = why;
	aop->error = error;
	aop->rx_halt = true;
}
#define a9_aop_fail(a, why, err) a9_aop_finish(a, PHASE_FAILED, why, err)
#define a9_aop_refuse(a, why) a9_aop_finish(a, PHASE_REFUSED, why, -16)
static void a9_aop_ep_fail(struct a9_aop *aop, struct a9_aop_ep *ep,
			 const char *why, int error)
{
	ep->state = AFK_FAILED;
	a9_aop_fail(aop, why, error);
}
static void event(unsigned long when, u64 word)
{
	assert(nevents < 32);
	events[nevents].when = when;
	events[nevents++].word = word;
}
static int a9_aop_send(struct a9_aop *aop, u8 ep, u64 payload)
{
	assert(a9_exp_tx_allowed(aop->experiment_stage, ep, payload) ||
	       (aop->cold_run && a9_sys_tx_allowed(ep, payload)));
	assert(ntx < 64);
	if (!ep && a9_mgmt_type(payload) == A9_MGMT_STARTEP && auto_system) {
		u8 started = (payload >> 32) & 0xff;
		if (started == 1 || started == 2 || started == 4)
			event(jiffies + 1, (u64)started << 56 | 1ULL << 52 | 1ULL << 44);
	}
	if (!ep && a9_mgmt_type(payload) == A9_MGMT_PING && auto_power)
		event(jiffies + 1, a9_mgmt_word(A9_MGMT_PING_ACK, (u32)payload));
	tx[ntx++] = ((u64)ep << 56) | payload;
	if (hello_step && !strcmp(aop->experiment_step, hello_step) &&
	    a9_mgmt_type(payload) != A9_MGMT_HELLO_REPLY) {
		hello_step = NULL;
		event(jiffies + 1, a9_mgmt_word(A9_MGMT_HELLO, (12U << 16) | 12));
	}
	if (!ep && a9_mgmt_type(payload) == A9_MGMT_HELLO_REPLY && auto_map &&
	    (!map_step || !strcmp(aop->experiment_step, map_step))) {
		event(jiffies + 1, a9_mgmt_word(A9_MGMT_EPMAP, 1));
		if (!partial_map)
			event(jiffies + 2, a9_mgmt_word(A9_MGMT_EPMAP,
					 (1ULL << 51) | (1ULL << 32) | 2));
	}
	if (!ep && a9_mgmt_type(payload) == A9_MGMT_AP_POWER && auto_power)
		event(jiffies + 1, a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, 0x20));
	if (!ep && payload == 0x00b0000000000020ULL && auto_power && aop->cold_run)
		event(jiffies + 2, a9_mgmt_word(A9_MGMT_AP_POWER_ACK, 0x20));
	return send_error;
}
static void queue_delayed_work(void *wq, int *work, int delay)
{
	(void)wq; (void)work; (void)delay;
	ping_queued++;
}
static void *dma_alloc_coherent(void *dev, size_t size, u64 *dma, int flags)
{
	(void)dev; (void)flags;
	coherent_allocs++;
	*dma = 0x800000000ULL;
	return malloc(size);
}
static void dma_free_coherent(void *dev, size_t size, void *va, u64 dma)
{
	(void)dev; (void)size; (void)dma;
	coherent_frees++;
	free(va);
}
static void a9_aop_inline_rx(struct a9_aop *aop);
static void a9_aop_stage3_mark(struct a9_aop *aop, const char *step)
{ (void)aop; (void)step; }
static void a9_aop_experiment_regs(struct a9_aop *aop, const char *label)
{
	(void)aop; (void)label;
}
static void a9_aop_experiment_unmask(struct a9_aop *aop)
{
	aop->mailbox_unmasked = true;
}
static void a9_aop_experiment_wake(struct a9_aop *aop)
{
	(void)aop;
	wake_calls++;
}
#define wait_event_timeout(q, cond, ms) do { \
	(void)(q); assert(!aop->cold_run); unsigned long end = jiffies + (ms); \
	while (!(cond) && jiffies < end) { a9_aop_inline_rx(aop); jiffies++; } \
} while (0)
/* Crash path itself is covered by result/state; the dump is MMIO-free text. */
static unsigned int crash_dumps;
static void a9_aop_crashlog(struct a9_aop *aop, const struct a9_aop_buffer *buf, u64 payload)
{ (void)aop; (void)buf; (void)payload; crash_dumps++; }
#include "experiment-functions.h"
static void a9_aop_inline_rx(struct a9_aop *aop)
{
	if (!aop->rx_halt && next_event < nevents && events[next_event].when <= jiffies)
		{
		u64 wire = events[next_event++].word;
		if (a9_aop_endpoint(wire))
			a9_aop_system_rx(aop, a9_aop_endpoint(wire), a9_aop_payload(wire));
		else
			a9_aop_mgmt(aop, wire);
	}
}
static struct a9_aop fresh(unsigned int stage)
{
	struct a9_aop aop = { .experiment_stage = stage, .phase = PHASE_PROBE };
	jiffies = 0; ntx = 0; ping_queued = 0; experiment_attempted = 0;
	send_error = 0;
	nevents = next_event = 0; auto_map = auto_power = true;
	map_step = hello_step = NULL; partial_map = false;
	cycle = false; auto_system = false;
	wake = wake_calls = 0;
	return aop;
}
static void handshake_tests(void)
{
	struct a9_aop aop;
	for (unsigned int stage = 2; stage <= 3; stage++) {
		for (unsigned int unsolicited = 0; unsolicited < 2; unsolicited++) {
			aop = fresh(stage);
			if (unsolicited)
				event(1, a9_mgmt_word(A9_MGMT_HELLO, (12U << 16) | 12));
			assert(a9_aop_experiment_handshake(&aop) == (stage == 3));
			assert(aop.handshake_complete && aop.epmap_chunks == 2);
			assert(aop.speculative_hello == !unsolicited);
			assert(ntx == 4 && !ping_queued);
			assert(tx[0] == 0x00200000000c000cULL);
			assert(aop.phase == (stage == 2 ? PHASE_STOPPED : PHASE_WAIT_SERVICE));
			assert(!a9_aop_experiment_handshake(&aop) && ntx == 4);
		}
	}
	aop = fresh(2); auto_map = false;
	assert(!a9_aop_experiment_handshake(&aop));
	assert(ntx == 6 && aop.phase == PHASE_FAILED && jiffies <= 15501);
	assert(aop.mailbox_unmasked && !aop.handshake_complete);
	assert(tx[0] == 0x00200000000c000cULL);
	assert(tx[1] == 0x0030000000000000ULL);
	assert(tx[2] == 0x0060000000000220ULL);
	assert(tx[3] == 0x00200000000c000cULL);
	assert(tx[4] == 0x00b0000000000020ULL);
	assert(tx[5] == 0x00200000000c000cULL);
	aop = fresh(2); auto_map = false; cycle = true;
	assert(!a9_aop_experiment_handshake(&aop));
	assert(ntx == 9 && jiffies <= 20001);
	assert(tx[6] == 0x0060000000000201ULL);
	assert(tx[7] == 0x0060000000000220ULL);
	assert(tx[8] == 0x00200000000c000cULL);
	aop = fresh(3); cycle = true; map_step = hello_step = "rewake220";
	assert(a9_aop_experiment_handshake(&aop));
	assert(!strcmp(aop.experiment_winner, "rewake220") && aop.device_hellos == 1);
	for (unsigned int stage = 2; stage <= 3; stage++) {
		/* A HELLO after the speculative reply is accepted, not rejected. */
		aop = fresh(stage); map_step = hello_step = "wake220";
		assert(a9_aop_experiment_handshake(&aop) == (stage == 3));
		assert(aop.device_hellos == 1 && aop.mailbox_unmasked);
		assert(!strcmp(aop.experiment_winner, "wake220") && ntx == 7);
		/* The last alternative can succeed without fabricating an IOP HELLO. */
		aop = fresh(stage); map_step = "hello-after-ap";
		assert(a9_aop_experiment_handshake(&aop) == (stage == 3));
		assert(!aop.device_hellos && aop.speculative_replies == 3);
		assert(!strcmp(aop.experiment_winner, "hello-after-ap") && ntx == 9);
	}
	/* Pre-HELLO power/ping ACKs are valid observations, not a handshake. */
	aop = fresh(2); auto_map = false;
	event(1, a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, 0x20));
	event(2, a9_mgmt_word(A9_MGMT_AP_POWER_ACK, 0x20));
	event(3, a9_mgmt_word(A9_MGMT_PING_ACK, 1));
	assert(!a9_aop_experiment_handshake(&aop));
	assert(aop.iop_ready && aop.ap_power_acked && ntx == 6);
	/* A partially started EPMAP must not be disturbed by the next alternative. */
	aop = fresh(2); partial_map = true;
	assert(!a9_aop_experiment_handshake(&aop) && ntx == 2);
	assert(!strcmp(aop.result, "failed-partial-epmap"));
	aop = fresh(2); send_error = -ETIMEDOUT;
	assert(!a9_aop_experiment_handshake(&aop) && ntx == 1);
	aop = fresh(2);
	event(1, a9_mgmt_word(A9_MGMT_HELLO, (11U << 16) | 11));
	assert(!a9_aop_experiment_handshake(&aop) && !ntx);
	aop = fresh(2); aop.hello = true;
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_EPMAP, 1ULL << 32));
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_EPMAP, 1));
	assert(aop.phase == PHASE_FAILED && ntx == 1);
	aop = fresh(2); aop.hello = true;
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_EPMAP, 8ULL << 32));
	assert(aop.phase == PHASE_FAILED && !ntx);
	aop = fresh(2); aop.hello = true;
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_HELLO, (12U << 16) | 12));
	assert(aop.phase != PHASE_FAILED && ntx == 1 && aop.device_hellos == 1);
	for (unsigned int selected = 1; selected <= 5; selected++) {
		aop = fresh(2); wake = selected; auto_map = false;
		assert(!a9_aop_experiment_handshake(&aop));
		assert(wake_calls == 1 && ntx == 5 && jiffies <= 13501);
		assert(!aop.mailbox_unmasked && !aop.handshake_complete);
		assert(!a9_aop_experiment_handshake(&aop) && wake_calls == 1 && ntx == 5);
		/* A real HELLO during the hardware-wake window chains stage 3. */
		aop = fresh(3); wake = selected;
		event(5001, a9_mgmt_word(A9_MGMT_HELLO, (12U << 16) | 12));
		assert(a9_aop_experiment_handshake(&aop));
		assert(wake_calls == 1 && aop.handshake_complete && ntx == 4);
		assert(!strcmp(aop.experiment_winner, "hardware-wake"));
		/* Passive success stops before touching hardware. */
		aop = fresh(2); wake = selected;
		event(1, a9_mgmt_word(A9_MGMT_HELLO, (12U << 16) | 12));
		assert(!a9_aop_experiment_handshake(&aop));
		assert(!wake_calls && aop.handshake_complete);
	}
}
static void grant_tests(void)
{
	struct a9_aop aop = fresh(3);
	struct a9_aop_ep ep = { .num = 0x21 };
	u8 pool[128];
	aop.pool = pool; aop.pool_size = sizeof(pool); aop.pool_pa = A9_EXP_POOL_PA;
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 0xcafe);
	assert(ep.nbufs == 1 && aop.pool_used == 64);
	assert(ep.bufs[0].va == pool && ep.bufs[0].dma == A9_EXP_POOL_PA);
	assert((tx[0] & A9_AOP_PA_MASK) == A9_EXP_POOL_PA);
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 0xbeef);
	assert(ep.nbufs == 2 && aop.pool_used == 128);
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 1);
	assert(aop.phase == PHASE_FAILED && ntx == 2 && ep.nbufs == 2);
	/* Failure retains both published slices and never frees/allocates coherent RAM. */
	assert(!coherent_allocs && !coherent_frees && aop.pool_used == 128);
	aop = fresh(3); memset(&ep, 0, sizeof(ep)); ep.num = 0x21;
	aop.pool = pool; aop.pool_size = sizeof(pool); aop.pool_pa = A9_EXP_POOL_PA;
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 1);
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 1);
	assert(aop.phase == PHASE_FAILED && ntx == 1 && aop.pool_used == 64);
	aop = fresh(3); memset(&ep, 0, sizeof(ep)); ep.num = 0x21;
	aop.pool = pool; aop.pool_size = sizeof(pool); aop.pool_pa = A9_EXP_POOL_PA;
	send_error = -ETIMEDOUT;
	a9_aop_getbuf(&aop, &ep, a9_afk_word(A9_AFK_GETBUF) | (1U << 16) | 1);
	assert(aop.phase == PHASE_FAILED && ep.nbufs == 1 && aop.pool_used == 64);
	assert(!coherent_frees && !coherent_allocs);
	size_t used = 0, off = 99;
	assert(a9_exp_alloc((size_t)-1, 128, &used, &off) && !used);
	assert(a9_exp_alloc(0, 128, &used, &off) && !used);
	assert(!a9_exp_alloc(1, 128, &used, &off) && used == 64 && off == 0);
	assert(!a9_exp_alloc(64, 128, &used, &off) && used == 128 && off == 64);
	assert(a9_exp_alloc(1, 128, &used, &off) && used == 128);
}
static void wait_ping_failure(struct a9_aop *aop)
{
	a9_aop_wait(aop, false, 7000);
}

static void system_tests(void)
{
	struct a9_aop aop;
	u8 pool[32768];
	u64 reply, pa;
	size_t size, used = 64, offset = 99;

	assert(!a9_sys_alloc(4096, sizeof(pool), &used, &offset));
	assert(offset == 4096 && used == 8192);
	assert(a9_sys_alloc(4096, 8192, &used, &offset) == -ENOSPC && used == 8192);
	used = (size_t)-1;
	assert(a9_sys_alloc(4096, used, &used, &offset) == -EINVAL);
	assert(!a9_sys_getbuf_reply(A9_EXP_POOL_PA, 4096, &reply));
	assert(reply == 0x0010100858000000ULL);
	assert(!a9_sys_getbuf(reply, &size, &pa));
	assert(size == 4096 && pa == A9_EXP_POOL_PA);
	assert(a9_sys_getbuf_reply(A9_EXP_POOL_PA + 64, 4096, &reply));
	assert(a9_sys_getbuf_reply(1ULL << 44, 4096, &reply));
	assert(!a9_sys_getbuf_reply(A9_EXP_POOL_PA, 0x100000, &reply));
	assert(!a9_sys_getbuf(reply, &size, &pa) && size == 0x100000);
	assert(!a9_sys_getbuf(3ULL << 52 | 2ULL << 44, &size, &pa) && size == 8192);
	assert(a9_sys_getbuf(1ULL << 52, &size, &pa));

	for (unsigned int run = 1; run <= 2; run++) {
		aop = fresh(3); aop.cold_run = run;
		aop.hello = aop.epmap_done = aop.handshake_complete = true;
		set_bit(1, aop.present); set_bit(2, aop.present); set_bit(4, aop.present);
		set_bit(9, aop.present); set_bit(0x21, aop.present);
		aop.pool = pool; aop.pool_pa = A9_EXP_POOL_PA;
		aop.pool_size = sizeof(pool); aop.pool_used = 64;
		auto_system = true; jiffies = 1000;
		/* inline_rx=false: boot must NOT wait for its own ordered queue. */
		assert(!aop.inline_rx && !a9_aop_system_start(&aop));
		assert(aop.system_ready && aop.system_grants == 3);
		assert(aop.pool_used == 16384 && aop.ping_sent_ns);
		a9_aop_inline_rx(&aop);
		assert(aop.ping_acks);
		assert(aop.phase == (run == 1 ? PHASE_SYSTEM_READY : PHASE_PROBE));
		assert(tx[0] == a9_mgmt_startep(1) && tx[1] == a9_mgmt_startep(2));
		assert(tx[2] == a9_mgmt_startep(4));
		assert(tx[3] == a9_mgmt_ap_power(true));
		assert(tx[4] == 0x0060000000000220ULL && tx[5] == 0x00b0000000000020ULL);
		for (unsigned int i = 0; i < ntx; i++)
			if (!a9_aop_endpoint(tx[i]) && a9_mgmt_type(tx[i]) == A9_MGMT_STARTEP)
				assert(((tx[i] >> 32) & 255) < 32);
	}
	/* Syslog fields/index are wider than a byte; bound before copying. */
	a9_aop_system_rx(&aop, 2, 8ULL << 52 | 300ULL << 24 | 1024);
	assert(aop.system[2].message_size == 300 && aop.system[2].entries == 1024);
	unsigned int before = ntx;
	a9_aop_system_rx(&aop, 2, 5ULL << 52 | 256);
	assert(ntx == before + 1 && tx[before] == (2ULL << 56 | 5ULL << 52 | 256));
	assert(aop.system[2].errors == 1); /* outside grant, but still ACKed */
	a9_aop_system_rx(&aop, 2, 8ULL << 52 | 300ULL << 24 | 1);
	a9_aop_system_rx(&aop, 2, 5ULL << 52); /* truncated log safely */
	assert(aop.system[2].acks == 2 && aop.system[2].errors == 1);
	before = ntx;
	a9_aop_system_rx(&aop, 4, 4ULL << 52);
	a9_aop_system_rx(&aop, 4, 12ULL << 52);
	assert(ntx == before); /* no ACK loops */
	a9_aop_system_rx(&aop, 4, 8ULL << 52 | 123);
	assert(tx[before] == (4ULL << 56 | 8ULL << 52));
	a9_aop_system_rx(&aop, 1, 4ULL << 52);
	assert(aop.iop_crashed && aop.phase == PHASE_FAILED && aop.rx_halt);

	for (unsigned int fault = 0; fault < 4; fault++) {
		aop = fresh(3); aop.cold_run = 1;
		aop.pool = pool; aop.pool_pa = A9_EXP_POOL_PA; aop.pool_size = sizeof(pool);
		if (fault == 0) aop.pool_size = 0;
		if (fault == 1) send_error = -ETIMEDOUT;
		a9_aop_system_rx(&aop, 2, 1ULL << 52 | 1ULL << 44 | (fault == 2 ? 0x1234 : 0));
		if (fault == 3) a9_aop_system_rx(&aop, 2, 1ULL << 52 | 1ULL << 44);
		assert(aop.phase == PHASE_FAILED && aop.system[2].errors == 1);
		assert(aop.pool_used == (fault == 1 || fault == 3 ? 4096 : 0));
	}
	aop = fresh(3); aop.cold_run = 2; aop.hello = true; auto_power = false;
	assert(a9_aop_system_start(&aop) == -ETIMEDOUT && !aop.system_ready);
	/* Power alone cannot mark conservative bring-up complete. */
	aop = fresh(3); aop.cold_run = 1; aop.hello = true;
	set_bit(2, aop.present);
	assert(a9_aop_system_start(&aop) == -ETIMEDOUT && !aop.system_ready);
	assert(aop.iop_ready && aop.ap_power_acked && !aop.system_grants);
	aop = fresh(3); aop.cold_run = 1; aop.handshake_complete = aop.hello = true;
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_HELLO, 12U << 16 | 12));
	assert(aop.phase == PHASE_FAILED && aop.rx_halt && !ntx);
	aop = fresh(3); aop.cold_run = 1; aop.hello = aop.epmap_done = true;
	a9_aop_mgmt(&aop, a9_mgmt_word(A9_MGMT_EPMAP, 0));
	assert(aop.phase == PHASE_FAILED && aop.rx_halt && !ntx);
	aop = fresh(3); aop.cold_run = 1; aop.handshake_complete = aop.hello = true;
	aop.phase = PHASE_SYSTEM_READY; aop.ping_ack_ns = 1; auto_power = false;
	wait_ping_failure(&aop);
	assert(aop.phase == PHASE_FAILED && !strcmp(aop.result, "failed-ping-timeout"));
	puts("PASS production stage3: ordered-queue synchronous RX, power/buffer gate, both modes, reserved grants, 24-bit syslog, no ACK loops, crash/failure and ping timeout");
}

int main(void)
{
	for (unsigned int type = 0; type < 16; type++) {
		bool allowed = type == 2 || type == 3 || type == 8 || type == 10;
		assert(a9_exp_tx_allowed(2, 0, (u64)type << 52) == allowed);
	}
	assert(a9_exp_tx_allowed(2, 0, 0x0060000000000220ULL));
	assert(a9_exp_tx_allowed(2, 0, 0x0060000000000201ULL));
	assert(!a9_exp_tx_allowed(2, 0, 0x0060000000000202ULL));
	assert(a9_exp_tx_allowed(2, 0, 0x00b0000000000020ULL));
	assert(!a9_exp_tx_allowed(2, 0, 0x00b0000000000201ULL));
	assert(!a9_exp_tx_allowed(2, 0x21, a9_afk_word(A9_AFK_GETBUF_ACK)));
	assert(!a9_exp_tx_allowed(3, 1, a9_afk_word(A9_AFK_GETBUF_ACK)));
	handshake_tests(); grant_tests(); system_tests();
	puts("PASS experimental production handshake/grants: one attempt, bounds, no page-allocator grants");
	return 0;
}
