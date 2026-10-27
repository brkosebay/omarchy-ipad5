/* SPDX-License-Identifier: GPL-2.0-only */
/* Production cold continuation: deterministic mutex race and fake AOP peer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "a9-aop-experiment.h"
#include "a9-aop-system.h"
#include "a9-aop-handoff.h"
#define __iomem
#include "a9-aop-reload.h"
#include "a9-aop-iio.h"

#define BIT(n) (1ULL << (n))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL
#define SZ_1M 0x100000U
#define AOP_CAL_FW "apple/aop/accel-calibration.bin"
#define ENOMEM 12
#define EIO 5
#define ENODEV 19
#define ENODATA 61
#define ENOTCONN 107
#define ETIMEDOUT 110
#define GFP_KERNEL 0
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define KERN_ERR ""
#define DUMP_PREFIX_OFFSET 0
#define print_hex_dump(...) ((void)0)
#define dma_rmb() ((void)0)
#define dma_wmb() ((void)0)
#define dma_mb() ((void)0)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define div_u64(a, b) ((a) / (b))
#define wake_up_all(x) ((void)(x))
#define msecs_to_jiffies(x) (x)
#define time_before(a, b) ((a) < (b))
#define set_bit(n, p) ((p)[(n) / 64] |= 1ULL << ((n) % 64))
#define test_bit(n, p) (!!((p)[(n) / 64] & (1ULL << ((n) % 64))))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define a9_aop_fail(a, why, err) a9_aop_finish(a, PHASE_FAILED, why, err)
#define a9_aop_refuse(a, why) a9_aop_finish(a, PHASE_REFUSED, why, -16)

typedef u64 dma_addr_t;
#include "stage3-types.h"

struct a9_aop {
	struct device *dev;
	void *wq;
	int lock, wait, ping_work, boot_work, poll_work, irq;
	bool irq_enabled, shutdown_quiesced, shutdown_power_requested;
	bool force_halted, force_drained;
	u32 force_halt_control;
	unsigned int stale_inbox, stale_outbox;
	u64 rx_words, tx_words;
	bool stopping, rx_halt, inline_rx, running_attach, cold;
	bool hello, epmap_done, handshake_complete, speculative_hello;
	u16 hello_min, hello_max;
	unsigned int experiment_stage, cold_run, epmap_seen, epmap_chunks;
	unsigned int device_hellos, speculative_replies;
	enum a9_aop_phase phase;
	const char *result, *experiment_step, *stage3_checkpoint;
	int error;
	u32 fault_control;
	u64 present[4];
	struct a9_aop_system system[A9_SYS_COUNT];
	bool system_ready, iop_crashed, ap_power_acked, iop_ready;
	u64 system_grants, system_words, unknown_mgmt;
	u32 ap_power_ack, iop_power, ping_seq;
	u64 ping_sent_ns, ping_ack_ns, ping_acks;
	struct a9_aop_ep eps[AOP_MAX_EPS];
	unsigned int neps;
	void *pool;
	u64 pool_pa;
	size_t pool_size, pool_used;
	struct a9_aop_service services[AOP_MAX_SERVICES];
	unsigned int nservices, poll_ms;
	int accel;
	u16 cmd_tag;
	bool cmd_pending;
	int cmd_status;
	unsigned int cal_sent, cal_unsupported;
	u32 cal_state;
	bool arot_sent, rate_set;
	u64 reports, report_errors, channel_mismatch, trailing, malformed;
	u64 truncated, doorbells, replies_seen;
	u8 reply_category;
	u16 reply_subtype, reply_tag;
	u32 reply_size, reply_word;
	struct a9_aop_motion motion;
	struct { u64 ns; struct a9_aop_motion motion; u32 size; u8 raw[64]; } samples[A9_EXP_SAMPLES];
	u8 first_report[64], rx_packet[AOP_PACKET_MAX], tx_packet[AOP_PACKET_MAX];
	u32 first_report_size;
	bool have_first_report;
	struct iio_dev *iio;
	/* Handoff replay only: certified layout and the record bookkeeping. */
	struct a9_aop_handoff ho;
	void *ext;
	u64 ext_dma, region_pa;
	size_t ext_size, region_size;
	void *region, *domains[AOP_DOMAINS];
	u32 *autoboot;
	bool cold_triggered;
	const char *experiment_winner;
	int wake_wrapper_errno;
	bool wake_wrapper_done;
	struct a9_aop_spi0 spi0;
};
struct firmware { const u8 *data; size_t size; };
struct iio_dev { unsigned int reports; bool valid; };
static struct iio_dev iio;
static struct firmware calibration;
static u8 pool[A9_EXP_POOL_SIZE];
static u64 clock_ns, tx[512];
static unsigned int ntx, property_count, cal_count;
static unsigned int configure_milestone;
static u32 property_keys[12];
static u32 firmware_cal_state, firmware_report_interval;
static bool stream_peer;
static bool quiesce, handoff;
#ifdef A9_SHUTDOWN_TEST
static bool shutdown_peer_tx(struct a9_aop *aop, u8 ep, u64 payload);
#endif
#ifdef A9_HANDOFF_TEST
static void a9_aop_handoff_classify(struct a9_aop *aop, u8 ep, u64 payload);
#endif
static u32 expected_cal_state;
static unsigned int drop_property, wrong_tag_property;
static bool peer_enabled, drop_reply, no_report, missing_cal, wrong_reply, bad_reply;
static bool captured_peer, captured_reply_seen, wrong_channel, wrong_subtype, short_reply;
static bool cold_cal_peer, unsupported_arot;
static unsigned int unsupported_property, error_property;
static bool race_after_rx;
static bool replay_ping_race;
static u64 sensors_ready_ns;
static u64 enabled_apps;
static unsigned int peer_init;
static bool defer_accel_service;
/* Endpoint sources are linked in reverse construction order. The frozen
 * HELLO SRAM image supplies names/list links; 2c is the captured first stall.
 */
static const u8 init_order[] = {
	0x2c, 0x2b, 0x29, 0x20, 0x26, 0x25, 0x21, 0x32, 0x2d,
	0x2a, 0x28, 0x31, 0x30, 0x27, 0x24, 0x23, 0x22,
};
static unsigned int halt_mode;
static void (*lock_waiter)(void);
static struct a9_aop *racing_aop;
static void inline_boot_worker(void);
#define jiffies ((unsigned long)(clock_ns / NSEC_PER_MSEC))
static u64 ktime_get_boottime_ns(void) { return clock_ns; }
static void usleep_range(int lo, int hi)
{ (void)lo; (void)hi; clock_ns += NSEC_PER_MSEC; }
static void mutex_lock(int *lock)
{
	if (lock_waiter) {
		void (*other_worker)(void) = lock_waiter;
		lock_waiter = NULL;
		other_worker(); /* Finish the owner and inline RX before acquiring. */
	}
	assert(!*lock);
	*lock = 1;
}
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static void queue_delayed_work(void *wq, int *work, unsigned int delay)
{ (void)wq; (void)work; (void)delay; }
static void a9_aop_stage3_mark(struct a9_aop *aop, const char *step)
{
	aop->stage3_checkpoint = step;
	if (!strcmp(step, "calibration-acked")) {
		assert(!configure_milestone && aop->cal_sent == 4 - aop->cal_unsupported);
		assert(aop->arot_sent == !unsupported_arot);
		configure_milestone = 1;
	} else if (!strcmp(step, "rate-acked")) {
		assert(configure_milestone == 1 && property_count == cal_count + 3 + !!firmware_cal_state && !aop->cmd_pending);
		configure_milestone = 2;
	} else if (!strcmp(step, "first-report")) {
		assert(configure_milestone == 2 && aop->have_first_report && aop->rate_set);
		configure_milestone = 3;
	} else if (!strcmp(step, "iio-registered")) {
		assert(configure_milestone == 3 && aop->iio && iio.valid);
		configure_milestone = 4;
	}
	if (replay_ping_race && !strcmp(step, "system-endpoints-started"))
		race_after_rx = true;
}
static void *dma_alloc_coherent(struct device *dev, size_t size, u64 *pa, int flags)
{ (void)dev; (void)size; (void)pa; (void)flags; abort(); }
static void dma_free_coherent(struct device *dev, size_t size, void *va, u64 pa)
{ (void)dev; (void)size; (void)va; (void)pa; abort(); }
static int request_firmware_direct(const struct firmware **fw, const char *name, struct device *dev)
{ (void)name; (void)dev; *fw = &calibration; return missing_cal ? -ENOENT : 0; }
static void release_firmware(const struct firmware *fw) { assert(fw == &calibration); }
struct iio_dev *a9_aop_accel_register(struct device *dev, const struct a9_aop_accel_config *config)
{
	struct a9_aop_motion motion;
	(void)dev;
	assert(config->report_interval_us == AOP_RATE_US);
	assert(config->firmware_applied_arot == !unsupported_arot);
	assert(!a9_spu_motion(config->first_report, &motion));
	assert(property_count == cal_count + 4 && !iio.valid);
	iio.valid = true;
	return &iio;
}
int a9_aop_accel_report(struct iio_dev *dev, const struct a9_spu_subpacket *sub)
{ assert(dev->valid && sub->subtype == 0xc2); dev->reports++; return 0; }
void a9_aop_accel_invalidate(struct iio_dev *dev) { dev->valid = false; }

static void a9_aop_inline_rx(struct a9_aop *aop);
static void peer_tx(struct a9_aop *aop, u8 ep, u64 payload);
static void capture_rx(struct a9_aop *aop, u8 ep, u64 payload);
/* Optional costs of a slow console (dmesg -n 8): each send prints its trace
 * and transcript lines, each received word its trace line. Off by default.
 */
static u64 send_cost_ns, rx_cost_ns;
static int a9_aop_send(struct a9_aop *aop, u8 ep, u64 payload)
{
	assert(aop->lock && ntx < ARRAY_SIZE(tx));
	assert(a9_exp_tx_allowed(aop->experiment_stage, ep, payload) ||
	       (aop->cold_run && a9_sys_tx_allowed(ep, payload)) ||
	       ((quiesce || handoff) && aop->inline_rx && aop->stopping &&
		a9_sys_shutdown_tx_allowed(ep, payload)));
	tx[ntx++] = (u64)ep << 56 | payload;
	aop->tx_words++;
	if (peer_enabled)
		peer_tx(aop, ep, payload);
	clock_ns += send_cost_ns;
	return 0;
}
#define wait_event_timeout(q, cond, ms) do { (void)(q); (void)(ms); assert(cond); } while (0)
/* Same synchronous wait as the production boot worker. */
/* Crash path itself is covered by result/state; the dump is MMIO-free text. */
static unsigned int crash_dumps;
static void a9_aop_crashlog(struct a9_aop *aop, const struct a9_aop_buffer *buf, u64 payload)
{ (void)aop; (void)buf; (void)payload; crash_dumps++; }
#include "stage3-wait.h"
#include "stage3-functions.h"

struct event { u64 ns, word; };
static struct event events[256];
static unsigned int head, tail;
static void enqueue(u64 ns, u8 ep, u64 payload)
{
	assert(tail < ARRAY_SIZE(events));
	unsigned int at = tail++;
	while (at > head && events[at - 1].ns > ns) {
		events[at] = events[at - 1];
		at--;
	}
	events[at] = (struct event){ ns, (u64)ep << 56 | payload };
}
static void receive(struct a9_aop *aop, u64 word)
{
	u8 ep = a9_aop_endpoint(word);
	u64 payload = a9_aop_payload(word);
#ifdef A9_HANDOFF_TEST
	/* Same position as the production dispatch hook. */
	if (aop->ho.waking)
		a9_aop_handoff_classify(aop, ep, payload);
#endif
	if (!ep)
		a9_aop_mgmt(aop, payload);
	else if (a9_aop_system_endpoint(ep))
		a9_aop_system_rx(aop, ep, payload);
	else {
		struct a9_aop_ep *e = a9_aop_find_ep(aop, ep);
		assert(e);
		if (captured_peer)
			capture_rx(aop, ep, payload);
		a9_aop_afk(aop, e, payload);
	}
}
/*
 * Optional AOP->AP outbox of outbox_depth words (8 on the device: stale
 * captures never exceed index 7). Words posted since the last drain share
 * it; the firmware loses every word posted while it is full (outbox_lost),
 * as the 17:35 burst lost all ACKs past the 8th. 0 = unbounded, the default.
 */
static unsigned int outbox_depth, outbox_lost;
static u64 last_drain_ns;
static void a9_aop_inline_rx(struct a9_aop *aop)
{
	unsigned int queued = 0, received = 0;

	mutex_lock(&aop->lock);
	while (!aop->rx_halt && head < tail && events[head].ns <= clock_ns) {
		if (outbox_depth && events[head].ns > last_drain_ns &&
		    queued++ >= outbox_depth) {
			outbox_lost++;
			head++;
			continue;
		}
		aop->rx_words++;
		received++;
		receive(aop, events[head++].word);
	}
	last_drain_ns = clock_ns;
	clock_ns += received * rx_cost_ns;
	mutex_unlock(&aop->lock);
	if (race_after_rx) {
		race_after_rx = false;
		lock_waiter = inline_boot_worker;
	}
}
static void ring_header(u8 *base, u32 size)
{
	memset(base, 0, size);
	put_unaligned_le32(size - A9_RING_HEADER, base);
	put_unaligned_le32(A9_RING_V7 | 6, base + 4);
}
static void peer_packet_ep(struct a9_aop *aop, u8 ep, u8 category, u16 subtype,
			   u16 tag, const u8 *payload, u32 size)
{
	struct a9_ring *ring = &a9_aop_find_ep(aop, ep)->rings[RING_RX];
	u8 packet[128] = { 2 };
	u32 next;
	put_unaligned_le32(size, packet + 16);
	packet[20] = 2; packet[21] = category;
	put_unaligned_le16(subtype, packet + 22);
	put_unaligned_le16(tag, packet + 24);
	memcpy(packet + 40, payload, size);
	assert(!a9_ring_write(ring, a9_ring_rptr(ring), a9_ring_wptr(ring),
			     (ep == 0x21 ? 0x10000007 : ep) +
			     (category == A9_SPU_REPLY && wrong_channel),
			     0, packet, 40 + size, &next));
	a9_ring_set_wptr(ring, next);
}
static void peer_packet(struct a9_aop *aop, u8 category, u16 subtype, u16 tag,
			const u8 *payload, u32 size)
{
	peer_packet_ep(aop, 0x21, category, subtype, tag, payload, size);
}
static void peer_service(struct a9_aop *aop, u8 ep)
{
	u8 service[40] = {};
	if (ep == 0x21)
		memcpy(service, "accel", 5);
	else
		snprintf((char *)service, 32, "fixture-%02x", ep);
	put_unaligned_le32(ep == 0x21 ? 0x10000007 : ep, service + 32);
	peer_packet_ep(aop, ep, 0, 0xc0, 0, service, sizeof(service));
}
static void peer_report(struct a9_aop *aop)
{
	u8 sample[22] = {};
	assert(firmware_cal_state && firmware_report_interval);
	put_unaligned_le16(aop->reports + 1, sample);
	put_unaligned_le32(65536, sample + 14);
	peer_packet(aop, 0, 0xc2, 0, sample, sizeof(sample));
}
#include "stage3-capture.h"
#include "stage3-cal-capture.h"
#include "stage3-stream-capture.h"
static void capture_rx(struct a9_aop *aop, u8 ep, u64 payload)
{
	if (a9_afk_type(payload) != A9_AFK_RX_NOTIFY)
		return;
	if (ep == 0x21 && property_count > 1)
		return; /* Replies after the first captured one come from the peer. */
	struct a9_ring *ring = &a9_aop_find_ep(aop, ep)->rings[RING_RX];
	for (unsigned int i = 0; i < ARRAY_SIZE(captured_rings); i++) {
		if (captured_rings[i].ep != ep || captured_rings[i].kind != RING_RX)
			continue;
		/* The first ep33 notify announces accel. The second publishes the
		 * captured 0x84 reply, before ep34's late START_ACK, exactly as saved.
		 */
		if (ep == 0x21 && a9_ring_wptr(ring) == 0x80) {
			assert(property_count == 1 && aop->cmd_pending && aop->cmd_tag == 1);
			captured_reply_seen = true;
			a9_ring_set_wptr(ring, 0xc0);
		} else {
			a9_ring_set_wptr(ring, ep == 0x21 ? 0x80 : captured_rings[i].wptr);
		}
		return;
	}
	abort();
}
static void peer_tx(struct a9_aop *aop, u8 ep, u64 payload)
{
	#ifdef A9_SHUTDOWN_TEST
	if (shutdown_peer_tx(aop, ep, payload))
		return;
	#endif
	u64 next = clock_ns + NSEC_PER_MSEC;
	u8 type = ep ? a9_afk_type(payload) : a9_mgmt_type(payload);
	if (!ep && type == A9_MGMT_PING) {
		enqueue(next, 0, a9_mgmt_word(A9_MGMT_PING_ACK, (u32)payload));
	} else if (captured_peer && ep >= 0x20 && type == A9_AFK_GETBUF_ACK) {
		struct a9_aop_ep *e = a9_aop_find_ep(aop, ep);
		for (unsigned int i = 0; i < ARRAY_SIZE(captured_rings); i++) {
			if (captured_rings[i].ep != ep)
				continue;
			u8 *base = (u8 *)e->bufs[0].va + captured_rings[i].offset;
			ring_header(base, captured_rings[i].size);
			if (captured_rings[i].kind == RING_RX)
				memcpy(base + A9_RING_HEADER, captured_rings[i].data, 256);
		}
	} else if (captured_peer && (ep != 0x21 || type != A9_AFK_DOORBELL)) {
		/* Replay already scheduled the real firmware's words. */
		if (!ep && type == A9_MGMT_STARTEP) enabled_apps |= BIT((payload >> 32) & 255);
	} else if (!ep && type == A9_MGMT_STARTEP) {
		u8 started = (payload >> 32) & 255;
		assert(started >= 0x20 && test_bit(started, aop->present));
		/* Handoff adopts certified grants without a reply. */
		assert(aop->system_ready && aop->system_grants + aop->ho.sys_adopted == 3);
		assert(!(enabled_apps & BIT(started)));
		enabled_apps |= BIT(started);
		/* f27c waits for the current source's routing enable, not accel's.
		 * With only 21 enabled the firmware stays here, pings still ACK.
		 */
		if (started == init_order[peer_init])
			enqueue(next < sensors_ready_ns ? sensors_ready_ns : next,
				started, a9_afk_word(A9_AFK_INIT) | 8);
	} else if (ep >= 0x20 && type == A9_AFK_INIT_ACK) {
		enqueue(next, ep, a9_afk_word(A9_AFK_GETBUF) | 0x1000001);
	} else if (ep >= 0x20 && type == A9_AFK_GETBUF_ACK) {
		struct a9_aop_ep *e = a9_aop_find_ep(aop, ep);
		u8 *memory = e->bufs[0].va;
		assert(ep == init_order[peer_init]);
		assert(e->bufs[0].dma == A9_EXP_POOL_PA + 0x12000 + peer_init * 0x4000);
		ring_header(memory, 0x2000); ring_header(memory + 0x2000, 0x2000);
		enqueue(next, ep, a9_afk_word(A9_AFK_TX_DESC) | 0x00800001);
		enqueue(next, ep, a9_afk_word(A9_AFK_RX_DESC) | 0x008000800001ULL);
	} else if (ep >= 0x20 && type == A9_AFK_START) {
		assert(ep == init_order[peer_init]);
		if (ep != 0x21 || !defer_accel_service)
			peer_service(aop, ep);
		enqueue(next, ep, a9_afk_word(A9_AFK_START_ACK));
		/* 11e74 waits for initialization completion before the next source.
		 * Do not synthesize concurrent INITs that mask this dependency.
		 */
		peer_init++;
		if (peer_init < ARRAY_SIZE(init_order)) {
			u8 following = init_order[peer_init];
			if (enabled_apps & BIT(following))
				enqueue(next + NSEC_PER_MSEC, following,
					a9_afk_word(A9_AFK_INIT) | 8);
		} else if (defer_accel_service) {
			peer_service(aop, 0x21);
			enqueue(next + NSEC_PER_MSEC, 0x21, a9_afk_word(A9_AFK_RX_NOTIFY));
		}
	} else if (ep == 0x21 && type == A9_AFK_DOORBELL) {
		struct a9_ring *ring = &a9_aop_find_ep(aop, ep)->rings[RING_TX];
		struct a9_ring_entry entry;
		struct a9_spu_subpacket sub;
		u8 packet[256], result[4] = {};
		u32 cursor, key;
		size_t at = 16;
		assert(!a9_ring_read(ring, a9_ring_wptr(ring), packet, sizeof(packet), &entry, &cursor));
		assert(entry.channel == 0x10000007 && a9_spu_next(packet, entry.length, &at, &sub) == 1);
		assert(sub.category == A9_SPU_NOTIFY && sub.subtype == 4);
		key = get_unaligned_le32(sub.payload + 4);
		assert(property_count < ARRAY_SIZE(property_keys));
		property_keys[property_count++] = key;
		if (key == 0x3d) {
			assert(sub.size == 12 && get_unaligned_le32(sub.payload + 8) == expected_cal_state);
		} else if (property_count > cal_count) {
			static const u32 keys[] = { 0x16, 0x5c, 0 };
			unsigned int i = property_count - cal_count - 1 - !!firmware_cal_state;
			assert(i < ARRAY_SIZE(keys) && key == keys[i] && sub.size == 12);
			assert(get_unaligned_le32(sub.payload + 8) == (i == 1 ? 0 : 100000));
		} else {
			struct a9_cal_entry entries[5];
			assert(a9_cal_parse(calibration.data, calibration.size, entries, 5) == (int)cal_count);
			struct a9_cal_entry *cal = &entries[property_count - 1];
			assert(key == cal->key && sub.size == cal->length + 8);
			assert(!memcmp(sub.payload + 8, cal->data, cal->length));
		}
		if (captured_peer && property_count == 1) {
			/* Compare the actual production TX including framing, key1a and
			 * all 20 private factory bytes to the captured command. */
			for (unsigned int i = 0; i < ARRAY_SIZE(captured_rings); i++)
				if (captured_rings[i].ep == ep && captured_rings[i].kind == RING_TX)
					assert(!memcmp(ring->base + A9_RING_HEADER, captured_rings[i].data, 0x80));
			ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
			return; /* The real reply arrives at captured_events[...]. */
		}
		if (cold_cal_peer && property_count <= 3) {
			struct a9_ring *rx = &a9_aop_find_ep(aop, ep)->rings[RING_RX];
			const u8 *reply = cold_cal_rx[property_count - 1];
			u32 next_cursor;
			/* The saved TX window begins at 0x80: compare complete TX2/3,
			 * including padding, framing, tags and the real factory bytes.
			 * RX1..3 are verbatim captured entries, not generated statuses.
			 */
			if (property_count >= 2)
				assert(!memcmp(a9_ring_entry(ring, entry.offset),
					       cold_cal_tx[property_count - 2], 128));
			assert(!a9_ring_write(rx, a9_ring_rptr(rx), a9_ring_wptr(rx),
				get_unaligned_le32(reply + 8), get_unaligned_le32(reply + 12),
				reply + 16, get_unaligned_le32(reply + 4), &next_cursor));
			a9_ring_set_wptr(rx, next_cursor);
			ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
			enqueue(clock_ns + cold_cal_delay_ns[property_count - 1], ep,
				a9_afk_word(A9_AFK_RX_NOTIFY));
			return;
		}
		if (stream_peer && key != 0x3d) {
			unsigned int old_tag = property_count - !!firmware_cal_state;
			struct a9_ring *rx = &a9_aop_find_ep(aop, ep)->rings[RING_RX];
			u32 next_cursor;
			if (old_tag >= 6) {
				u8 expected[128];
				memcpy(expected, stream_tx[old_tag - 6], sizeof(expected));
				/* Added 3d shifts sequence/tag by one. Historical replay compares
				 * verbatim bytes; corrected order compares only with that retag. */
				put_unaligned_le16(old_tag - 1 + !!firmware_cal_state, expected + 17);
				put_unaligned_le16(sub.tag, expected + 40);
				assert(!memcmp(a9_ring_entry(ring, entry.offset), expected, 128));
			}
			if (old_tag >= 4) {
				u8 reply[64];
				memcpy(reply, stream_rx[old_tag - 4], sizeof(reply));
				put_unaligned_le16(old_tag + !!firmware_cal_state, reply + 17);
				put_unaligned_le16(sub.tag, reply + 40);
				assert(!a9_ring_write(rx, a9_ring_rptr(rx), a9_ring_wptr(rx),
					get_unaligned_le32(reply + 8), get_unaligned_le32(reply + 12),
					reply + 16, get_unaligned_le32(reply + 4), &next_cursor));
				a9_ring_set_wptr(rx, next_cursor);
			} else {
				/* RX1..3 fell outside this capture's window; statuses are in dmesg. */
				put_unaligned_le32(old_tag == 3 ? A9_SPU_UNSUPPORTED : 0, result);
				peer_packet(aop, A9_SPU_REPLY, 0x84, sub.tag, result, sizeof(result));
			}
			if (key == 0) {
				firmware_report_interval = get_unaligned_le32(sub.payload + 8);
				if (firmware_cal_state && !no_report)
					peer_report(aop);
			}
			ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
			enqueue(clock_ns + stream_delay_ns[old_tag - 1], ep,
				a9_afk_word(A9_AFK_RX_NOTIFY));
			return;
		}
		ring->cursor = cursor; a9_ring_set_rptr(ring, cursor);
		if (!drop_reply && property_count != drop_property) {
			if (bad_reply || property_count == error_property)
				put_unaligned_le32(0xe00002bc, result);
			if (property_count == unsupported_property)
				put_unaligned_le32(A9_SPU_UNSUPPORTED, result);
			peer_packet(aop, A9_SPU_REPLY, wrong_subtype ? 4 : 0x84,
				    sub.tag + wrong_reply + (property_count == wrong_tag_property),
				    result, short_reply ? 3 : sizeof(result));
			if (key == 0x3d && !get_unaligned_le32(result))
				firmware_cal_state = get_unaligned_le32(sub.payload + 8);
			if (key == 0)
				firmware_report_interval = get_unaligned_le32(sub.payload + 8);
			if ((key == 0 || key == 0x3d) && firmware_cal_state &&
			    firmware_report_interval && !no_report)
				peer_report(aop);
			enqueue(next, ep, a9_afk_word(A9_AFK_RX_NOTIFY));
		}
	}
}

static struct a9_aop fresh(unsigned int run)
{
	struct a9_aop aop = { .experiment_stage = 3, .cold_run = run, .accel = -1,
		.phase = PHASE_WAIT_HELLO, .pool = pool, .pool_pa = A9_EXP_POOL_PA,
		.pool_size = sizeof(pool), .pool_used = 0xc000 };
	memset(pool, 0, sizeof(pool)); memset(&iio, 0, sizeof(iio));
	clock_ns = 33322381000ULL; ntx = head = tail = property_count = 0;
	configure_milestone = 0;
	peer_enabled = drop_reply = no_report = missing_cal = wrong_reply = bad_reply = false;
	captured_peer = captured_reply_seen = wrong_channel = wrong_subtype = short_reply = false;
	cold_cal_peer = unsupported_arot = false;
	unsupported_property = error_property = 0;
	firmware_cal_state = firmware_report_interval = 0; expected_cal_state = 2;
	stream_peer = false; drop_property = wrong_tag_property = 0;
	enabled_apps = peer_init = 0; defer_accel_service = false;
	replay_ping_race = true; sensors_ready_ns = 0;
	lock_waiter = NULL; race_after_rx = false;
	mutex_lock(&aop.lock);
	receive(&aop, 0x00100000000c000cULL);
	receive(&aop, 0x0080000000000017ULL);
	receive(&aop, 0x0088000100073fffULL);
	aop.handshake_complete = true;
	mutex_unlock(&aop.lock);
	assert(aop.hello && aop.epmap_done && aop.neps == 17);
	return aop;
}
static void halt_while_waiting(void)
{
	if (halt_mode == 0) racing_aop->stopping = true;
	if (halt_mode == 1) racing_aop->rx_halt = true;
	if (halt_mode == 2) racing_aop->phase = PHASE_REFUSED;
}

/* Real dmesg sequence, 20261025-1629 full failure. Wire reception of the
 * ep4 request and ping ACK precedes their serialized dispatch under lock.
 * The background tick waits since before the inline boot tick sends ping 1.
 */
static void inline_boot_worker(void)
{
	struct a9_aop *aop = racing_aop;
	clock_ns = 33378622000ULL;
	mutex_lock(&aop->lock); receive(aop, 0x0110100000000000ULL); mutex_unlock(&aop->lock);
	clock_ns = 33378794000ULL;
	a9_aop_ping_tick(aop);
	assert(tx[ntx - 1] == 0x0030002100000001ULL);
	clock_ns = 33379335000ULL;
	enqueue(clock_ns, 4, 0x10400000000000ULL);
	enqueue(33379341000ULL, 0, 0x40000000000001ULL);
	clock_ns = 33379541000ULL;
	a9_aop_inline_rx(aop);
	assert(aop->ping_acks == 1 && aop->system_grants == 2);
	assert(tx[ntx - 1] == 0x041040085800d000ULL);
	clock_ns = 33379560000ULL;
}
static struct a9_aop replay_system(unsigned int run)
{
	struct a9_aop aop = fresh(run);
	/* Exact system STARTEPs/power words precede the captured grant order.
	 * Force the first tick to wait while the other worker runs the capture. */
	racing_aop = &aop; clock_ns = 33376385000ULL;
	enqueue(33494365000ULL, 0, a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, 0x20));
	enqueue(33494386000ULL, 2, 0x80000050000010ULL);
	enqueue(33494390000ULL, 2, 0x10100000000000ULL);
	enqueue(33505340000ULL, 0, a9_mgmt_word(A9_MGMT_AP_POWER_ACK, 0x20));
	int ret = a9_aop_system_start(&aop);
	if (ret)
		fprintf(stderr, "captured ping race: result=%s ping_seq=%u ping_acks=%llu\n",
			aop.result, aop.ping_seq, (unsigned long long)aop.ping_acks);
	assert(!ret && aop.phase != PHASE_FAILED && aop.ping_seq == 1 && aop.ping_acks == 1);
	assert(aop.ping_sent_ns == 33378794000ULL && aop.ping_ack_ns == 33379541000ULL);
	assert(aop.system_ready && aop.system_grants == 3 && aop.pool_used == 0x12000);
	assert(aop.system[1].buffer.dma == A9_EXP_POOL_PA + 0xc000);
	assert(aop.system[4].buffer.dma == A9_EXP_POOL_PA + 0xd000);
	assert(aop.system[2].buffer.dma == A9_EXP_POOL_PA + 0x11000);
	assert(aop.system[2].entries == 16 && aop.system[2].message_size == 80);
	return aop;
}
/* Full ping-fix device capture, 20261025-1750. Real receive timestamps,
 * buffer words and IOReport completions; the peer substitutes the AP's new
 * startup decisions after readiness. No property or INIT is pre-announced.
 */
static struct a9_aop replay_enable_system(void)
{
	struct a9_aop aop = fresh(2);
	static const struct event captured[] = {
		{ 29030282000ULL, 0x0110100000000000ULL },
		{ 29033247000ULL, 0x0410400000000000ULL },
		{ 29033247000ULL, 0x0040000000000001ULL },
		{ 29035262000ULL, 0x04c0000000000000ULL },
		{ 29035273000ULL, 0x0480000000000000ULL },
		{ 29039419000ULL, 0x0480000000000000ULL },
		{ 29042360000ULL, 0x0480000000000000ULL },
		{ 29044059000ULL, 0x0480000000000000ULL },
		{ 29045268000ULL, 0x0480000000000000ULL },
		{ 29046246000ULL, 0x0480000000000000ULL },
		{ 29047309000ULL, 0x0480000000000000ULL },
		{ 29048255000ULL, 0x0480000000000000ULL },
		{ 29049260000ULL, 0x0480000000000000ULL },
		{ 29051277000ULL, 0x0480000000000000ULL },
		{ 29052249000ULL, 0x0070000000000020ULL },
		{ 29052254000ULL, 0x0280000050000010ULL },
		{ 29052257000ULL, 0x0210100000000000ULL },
		{ 29055275000ULL, 0x00b0000000000020ULL },
		{ 29055290000ULL, 0x0480000000000000ULL },
	};
	replay_ping_race = false; clock_ns = 29026457000ULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(captured); i++)
		enqueue(captured[i].ns, a9_aop_endpoint(captured[i].word),
			a9_aop_payload(captured[i].word));
	assert(!a9_aop_system_start(&aop));
	assert(aop.system_ready && aop.system_grants == 3 && aop.ping_acks == 1);
	assert(aop.pool_used == 0x12000 && !aop.nservices && !aop.iio);
	/* Saved slot contents, including firmware's 24-byte context truncation. */
	static const char * const messages[] = {
		"mpu6700: mpu6900 device ID 0x10 found.",
		"ak8963::probe -- Found device with ID: 0x48", "AKM963 device found.",
		"BMP280::probe -- Unknown device ID 0x0", "BMP280 device NOT found.",
		"BMP284 device found", "UartBasebandService start: baseband isn't present",
		"waiting for endpoint enable, _routing=2c",
	};
	static const u64 log_ns[] = {
		29166259000ULL, 29167270000ULL, 29167278000ULL, 29169253000ULL,
		29169265000ULL, 29171272000ULL, 29171284000ULL,
	};
	u8 *logs = aop.system[2].buffer.va;
	for (unsigned int i = 0; i < ARRAY_SIZE(messages); i++) {
		memcpy(logs + i * 112 + 8, i == 7 ? "nLoopEventSource.cpp:49" : "sensor-probe", i == 7 ? 22 : 12);
		strcpy((char *)logs + i * 112 + 32, messages[i]);
		if (i < ARRAY_SIZE(log_ns))
			enqueue(log_ns[i], 2, a9_mgmt_word(5, i));
	}
	sensors_ready_ns = 29293277000ULL;
	return aop;
}
static void load_calibration(const char *path)
{
	static u8 synthetic[12 + 4 * 12];
	struct a9_cal_entry entries[5];
	if (strcmp(path, "-")) {
		FILE *file = fopen(path, "rb");
		assert(file && !fseek(file, 0, SEEK_END));
		long size = ftell(file);
		assert(size > 0 && !fseek(file, 0, SEEK_SET));
		u8 *data = malloc(size);
		assert(data && fread(data, 1, size, file) == (size_t)size);
		fclose(file); calibration = (struct firmware){ data, size };
	} else {
		/* Synthetic packet fixture only; never packaged as device calibration. */
		static const u32 keys[] = { 0x1a, 0x0b, 0x18, 0x19 };
		memcpy(synthetic, "A9AC", 4); put_unaligned_le32(1, synthetic + 4);
		put_unaligned_le32(4, synthetic + 8);
		for (unsigned int i = 0; i < 4; i++) {
			put_unaligned_le32(keys[i], synthetic + 12 + i * 12);
			put_unaligned_le32(4, synthetic + 16 + i * 12);
		}
		calibration = (struct firmware){ synthetic, sizeof(synthetic) };
	}
	assert(a9_cal_parse(calibration.data, calibration.size, entries, 5) == 4);
	cal_count = 4;
}
int main(int argc, char **argv)
{
	assert(argc == 2); load_calibration(argv[1]);
	struct a9_aop aop;
	/* New full continuation against the latest rate-ACK/no-report capture.
	 * Reports require the firmware's nonzero calibration-state gate. */
	aop = replay_system(2); peer_enabled = stream_peer = unsupported_arot = true;
	continue_boot(&aop);
	if (aop.phase != PHASE_STREAMING)
		fprintf(stderr, "20261025-1922 report-gate replay: result=%s properties=%u state=%u reports=%llu\n",
			aop.result, property_count, firmware_cal_state, (unsigned long long)aop.reports);
	assert(aop.phase == PHASE_STREAMING && aop.cal_state == 2 && firmware_cal_state == 2);
	assert(property_count == 8 && property_keys[4] == 0x3d && property_keys[5] == 0x16);
	assert(aop.cal_sent == 3 && aop.cal_unsupported == 1 && !aop.arot_sent && iio.valid);
	for (unsigned int i = 0; i < 3; i++) {
		peer_report(&aop);
		mutex_lock(&aop.lock); receive(&aop, 0x2185000000000000ULL); mutex_unlock(&aop.lock);
	}
	assert(aop.reports == 4 && iio.reports == 3);
	puts("PASS latest rate capture + calibration-state ACK -> first report -> IIO -> increasing reports (3d ACK/reports modeled, rate frames retagged)");
	/* Replay the original seven transactions without 3d. This must remain
	 * silent with consumed TX/RX, then 3d=2 alone opens the modeled gate. */
	aop = replay_system(2); peer_enabled = stream_peer = unsupported_arot = true;
	mutex_lock(&aop.lock);
	for (unsigned int i = 0; i < aop.neps; i++) {
		aop.eps[i].state = AFK_WAIT_INIT;
		assert(!a9_aop_send(&aop, 0, a9_mgmt_startep(aop.eps[i].num)));
	}
	mutex_unlock(&aop.lock);
	assert(a9_aop_wait(&aop, aop.accel >= 0, AOP_SERVICE_MS));
	struct a9_cal_entry legacy[5];
	assert(a9_cal_parse(calibration.data, calibration.size, legacy, 5) == 4);
	aop.phase = PHASE_CONFIGURE;
	for (unsigned int i = 0; i < 4; i++)
		assert(a9_aop_property(&aop, legacy[i].key, legacy[i].data, legacy[i].length) ==
		       (legacy[i].key == 0x18 ? -EOPNOTSUPP : 0));
	aop.rate_set = true; aop.phase = PHASE_WAIT_REPORT;
	assert(!a9_aop_set_rate(&aop, 100000, 0, 100000));
	assert(!a9_aop_wait(&aop, aop.have_first_report, AOP_REPORT_MS));
	struct a9_aop_ep *ep = a9_aop_find_ep(&aop, 0x21);
	assert(property_count == 7 && aop.replies_seen == 7 && !aop.reports && !iio.valid);
	assert(!firmware_cal_state && firmware_report_interval == 100000);
	assert(a9_ring_rptr(&ep->rings[RING_TX]) == a9_ring_wptr(&ep->rings[RING_TX]));
	assert(a9_ring_rptr(&ep->rings[RING_RX]) == a9_ring_wptr(&ep->rings[RING_RX]));
	assert(!a9_aop_property32(&aop, 0x3d, 2) && aop.have_first_report && aop.reports == 1);
	puts("PASS exact seven-command rate-ACK sequence: TX6/7 and RX4..7 verbatim, measured delays, empty queues, no reports for 3s; subsequent 3d=2 opens modeled report gate");
	/* Actual fourth-run calibration sequence: 1a/20 -> 0, 0b/36 -> 0,
	 * 18/16 -> e00002c7. Transport startup is modeled; TX2/3 and RX1..3
	 * are captured bytes and retain their measured TX-to-RX delays.
	 * Later key19, calibration-state/rate ACKs and motion reports remain modeled.
	 */
	aop = replay_system(2); peer_enabled = cold_cal_peer = unsupported_arot = true;
	continue_boot(&aop);
	if (aop.phase != PHASE_STREAMING)
		fprintf(stderr, "20261025-1853 calibration replay: result=%s cal_acked=%u unsupported=%u tag=%u last=%08x\n",
			aop.result, aop.cal_sent, aop.cal_unsupported, aop.cmd_tag, aop.reply_word);
	assert(aop.phase == PHASE_STREAMING && aop.cal_sent == 3 && aop.cal_unsupported == 1);
	assert(!aop.arot_sent && aop.rate_set && aop.reports == 1 && iio.valid);
	assert(property_count == 8 && aop.replies_seen == 8 && configure_milestone == 4);
	assert(property_keys[2] == 0x18 && property_keys[3] == 0x19);
	peer_report(&aop);
	mutex_lock(&aop.lock); receive(&aop, 0x2185000000000000ULL); mutex_unlock(&aop.lock);
	assert(aop.reports == 2 && iio.reports == 1);
	puts("PASS captured DFU 20261025-1853 calibration: exact TX2/3, RX1/2 success, RX3 unsupported18; 3 ACKs/1 skipped/ARot false -> rate -> first report -> IIO -> increasing reports (startup/later replies/reports modeled)");
	/* Exact third run: variable-sized AFK grants, all 12 real services and
	 * their true channels, orientation report, key1a notify and reply0x84.
	 * Only replies AFTER the captured success and motion reports are modeled.
	 */
	aop = replay_system(2); peer_enabled = captured_peer = true;
	replay_ping_race = false; head = tail = 0;
	clock_ns = 15518013000ULL;
	aop.ping_sent_ns = aop.ping_ack_ns = 15474102589ULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(captured_events); i++)
		enqueue(captured_events[i].ns, a9_aop_endpoint(captured_events[i].word),
			a9_aop_payload(captured_events[i].word));
	continue_boot(&aop);
	if (aop.phase != PHASE_STREAMING)
		fprintf(stderr, "20261025-1805 configure replay: result=%s cal_acked=%u tag=%u last_reply=%02x/%x/%u/%08x\n",
			aop.result, aop.cal_sent, aop.cmd_tag, aop.reply_category,
			aop.reply_subtype, aop.reply_tag, aop.reply_word);
	assert(captured_reply_seen && aop.phase == PHASE_STREAMING);
	assert(aop.nservices == 12 && aop.pool_used == 0x186000);
	assert(aop.services[aop.accel].channel == 0x10000007);
	assert(aop.cal_sent == 4 && aop.arot_sent && aop.rate_set && aop.reports == 1 && iio.valid);
	assert(property_count == 8 && aop.replies_seen == 8 && aop.reply_subtype == 0x84);
	assert(configure_milestone == 4);
	puts("PASS captured 20261025-1805 rings/transcript: key1a consumed, reply20/84/tag1/status0; all four cal ACKs/ARot, rates, first report, IIO streaming (later replies/reports modeled)");
	aop = replay_enable_system(); peer_enabled = true;
	continue_boot(&aop);
	if (aop.phase != PHASE_STREAMING)
		fprintf(stderr, "20261025-1750 firmware replay: result=%s enabled=%llx current=%02x ping_acks=%llu\n",
			aop.result, (unsigned long long)enabled_apps, init_order[peer_init],
			(unsigned long long)aop.ping_acks);
	assert(aop.phase == PHASE_STREAMING && aop.cal_sent == 4 && aop.arot_sent);
	assert(a9_aop_wait(&aop, peer_init == 17, AOP_SERVICE_MS));
	assert(aop.system[2].acks == 7 && !aop.system[2].errors);
	for (unsigned int run = 1; run <= 2; run++) {
		aop = replay_system(run);
		assert(!aop.system[1].errors && !aop.system[2].errors && !aop.system[4].errors);
		for (unsigned int i = 0; i < ntx; i++)
			if (a9_mgmt_type(tx[i]) == A9_MGMT_STARTEP)
				assert(((tx[i] >> 32) & 255) < 32);
		if (run == 1) {
			assert(aop.phase == PHASE_SYSTEM_READY && !aop.iio);
			continue;
		}
		peer_enabled = true;
		continue_boot(&aop);
		if (aop.phase != PHASE_STREAMING)
			fprintf(stderr, "serialized firmware: result=%s enabled=%llx current=%02x ping_acks=%llu\n",
				aop.result, (unsigned long long)enabled_apps, init_order[peer_init],
				(unsigned long long)aop.ping_acks);
		assert(aop.phase == PHASE_STREAMING && iio.valid && aop.cal_sent == 4);
		assert(aop.arot_sent && aop.rate_set && aop.reports == 1 && aop.iio == &iio);
		assert(a9_aop_find_ep(&aop, 0x21)->state == AFK_RUNNING);
		assert(a9_aop_find_ep(&aop, 0x2c)->state == AFK_RUNNING);
		for (unsigned int i = 0; i < aop.neps; i++)
			assert(enabled_apps & BIT(aop.eps[i].num));
		assert(property_count == 8 && property_keys[0] == 0x1a && property_keys[3] == 0x19);
		peer_report(&aop);
		mutex_lock(&aop.lock); receive(&aop, 0x2185000000000000ULL); mutex_unlock(&aop.lock);
		assert(aop.reports == 2 && iio.reports == 1);
	}
	/* Reproduce the device's 21-only STARTEP, no INIT/service, healthy pings.
	 * The old driver waited here for 5s. This peer must expose that failure.
	 */
	aop = replay_enable_system(); peer_enabled = true;
	mutex_lock(&aop.lock);
	a9_aop_find_ep(&aop, 0x21)->state = AFK_WAIT_INIT;
	assert(!a9_aop_send(&aop, 0, a9_mgmt_startep(0x21)));
	mutex_unlock(&aop.lock);
	for (unsigned int i = 0; i < 5; i++)
		enqueue(sensors_ready_ns + i * NSEC_PER_SEC, 2, a9_mgmt_word(5, 7));
	assert(!a9_aop_wait(&aop, aop.accel >= 0, AOP_SERVICE_MS));
	assert(!peer_init && !aop.nservices && !aop.cal_sent && !aop.reports && !aop.iio);
	assert(aop.ping_acks >= 5 && aop.phase != PHASE_FAILED);
	assert(aop.system[2].acks == 12 && !aop.system[2].errors);
	/* All 17 announcements must fit even if accel is last on the ring. */
	aop = replay_system(2); peer_enabled = defer_accel_service = true;
	continue_boot(&aop);
	assert(aop.phase == PHASE_STREAMING && aop.nservices == 17 && aop.accel == 16);
	assert(peer_init == 17 && aop.pool_used == 0x56000 && aop.cal_sent == 4);
	/* Recent ACK doesn't hide an actually missing subsequent ACK. */
	aop = replay_system(2);
	clock_ns = aop.ping_ack_ns + (AOP_PING_TIMEOUT_MS + 1) * NSEC_PER_MSEC;
	a9_aop_ping_tick(&aop);
	assert(aop.phase == PHASE_FAILED && !strcmp(aop.result, "failed-ping-timeout"));
	unsigned int before = ntx;
	clock_ns += AOP_PING_MS * NSEC_PER_MSEC; a9_aop_ping_tick(&aop);
	assert(ntx == before + 1 && !aop.rx_halt);
	/* Stop/refusal may arrive while the background tick waits on RX. */
	for (halt_mode = 0; halt_mode < 3; halt_mode++) {
		aop = replay_system(2); racing_aop = &aop;
		clock_ns += 6 * NSEC_PER_SEC;
		before = ntx; lock_waiter = halt_while_waiting;
		a9_aop_ping_tick(&aop);
		assert(ntx == before && !aop.lock);
	}
	for (unsigned int mode = 0; mode < 8; mode++) {
		aop = replay_system(2); peer_enabled = true;
		drop_reply = mode == 0; wrong_reply = mode == 1;
		missing_cal = mode == 2; no_report = mode == 3; bad_reply = mode == 4;
		wrong_channel = mode == 5; wrong_subtype = mode == 6; short_reply = mode == 7;
		continue_boot(&aop);
		assert(aop.phase == PHASE_FAILED && !iio.valid && !aop.iio);
		assert(!strcmp(aop.result, mode == 3 ? "failed-no-report" : "failed-configure"));
		assert(property_count == (mode == 2 ? 0 : mode == 3 ? 8 : 1));
		assert(aop.error == (mode == 2 ? -ENOENT : mode == 4 ? -EREMOTEIO :
				     mode == 7 ? -EMSGSIZE : -ETIMEDOUT));
	}
	/* A non-unsupported ARot error still fails; unsupported rate is fatal.
	 * Optional calibration does not weaken the sampling contract.
	 */
	for (unsigned int mode = 0; mode < 2; mode++) {
		aop = replay_system(2); peer_enabled = true;
		error_property = mode == 0 ? 3 : 0;
		unsupported_property = mode == 1 ? 6 : 0;
		continue_boot(&aop);
		assert(aop.phase == PHASE_FAILED && !strcmp(aop.result, "failed-configure"));
		assert(aop.error == (mode ? -EOPNOTSUPP : -EREMOTEIO));
		assert(property_count == (mode ? 6 : 3) && !aop.cal_unsupported && !iio.valid);
	}
	/* A rejected required calibration never claims complete state. iOS's
	 * state1 allows explicitly incomplete calibration without fabricated data. */
	aop = replay_system(2); peer_enabled = true;
	unsupported_property = 2; expected_cal_state = 1;
	continue_boot(&aop);
	assert(aop.phase == PHASE_STREAMING && aop.cal_state == 1 && firmware_cal_state == 1);
	assert(aop.cal_sent == 3 && aop.cal_unsupported == 1 && aop.arot_sent && iio.valid);
	/* State publication is mandatory: timeout, unsupported and wrong-tag
	 * ACKs must fail before any rate command or IIO registration. */
	for (unsigned int mode = 0; mode < 3; mode++) {
		aop = replay_system(2); peer_enabled = true;
		drop_property = mode == 0 ? 5 : 0;
		unsupported_property = mode == 1 ? 5 : 0;
		wrong_tag_property = mode == 2 ? 5 : 0;
		continue_boot(&aop);
		assert(aop.phase == PHASE_FAILED && !strcmp(aop.result, "failed-configure"));
		assert(aop.error == (mode == 1 ? -EOPNOTSUPP : -ETIMEDOUT));
		assert(property_count == 5 && !aop.rate_set && !aop.cal_state && !iio.valid);
	}
	puts("PASS production stage3: captured 21-only/2c-enable stall with healthy pings; all 17 advertised enables and serialized AFK v8/rings, accel-last discovery, real calibration/ARot/rate/report/IIO; ping race and genuine timeout retained");
	if (strcmp(argv[1], "-")) free((void *)calibration.data);
	return 0;
}
