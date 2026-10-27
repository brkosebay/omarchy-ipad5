/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Opt-in AOP sleep/wake handoff (aop_handoff=sleep): one versioned record in
 * the last 16 KiB page of the reserved stage-3 pool. The allocator capacity
 * excludes that page, so the proven ext/system/AFK offsets are unchanged.
 * The outgoing kernel writes it after AP 10/IOP 201 ACKs and a stable pool,
 * without halting the AOP. The next kernel validates it against live RAM,
 * SRAM and wrapper state, consumes it once and wakes the resident firmware.
 * The same page carries a shutdown transcript log for aop_quiesce=1 and
 * aop_handoff=sleep: written line by line during .shutdown, independent of
 * printk console levels, and replayed once by the next kernel's probe.
 * Pure functions on plain memory: the host replay compiles this unchanged.
 * Fields are AP-native little-endian; the digest covers all earlier bytes.
 */
#ifndef _A9_AOP_HANDOFF_H
#define _A9_AOP_HANDOFF_H

#include "a9-aop-system.h"

#define A9_HO_MAGIC		0x4f483941U	/* "A9HO" */
#define A9_HO_VERSION		1U
#define A9_HO_PAGE		0x4000U
/* Boot + text up to the tag table (0x389b8): zero changed words from load
 * through HELLO. The table itself is excluded: its only direct reader
 * (0x1000b5c, STKG) is a load, but a computed-pointer tag update must
 * never turn a resident image into a refused hop.
 */
#define A9_HO_SRAM_SPAN		0x389b8U
#define A9_HO_MAX_AFK		96
#define A9_HO_DIGEST		32
#define A9_HO_GENERATION_MAX	0xffffU
#define A9_HO_ACCEL_EP		0x21
/* Tail page: record at 0, shutdown transcript log from 0x800. */
#define A9_HO_LOG_OFFSET	0x800U
#define A9_SL_MAGIC		0x4c533941U	/* "A9SL" */
#define A9_SL_VERSION		1U

/* Firmware 0x220 -> internal 0x21 (on), as iOS b71fc4 sends on resume. */
#define A9_HO_IOP_WAKE		0x0060000000000220ULL
#define A9_HO_AP_ON		0x00b0000000000020ULL

/* Wrapper bits, duplicated from the driver for the pure live check. */
#define A9_HO_MBOX_EMPTY	0x00020000U
#define A9_HO_MBOX_FAULTS	0x000c0000U
#define A9_HO_OUTBOX_ENABLE	0x00000001U

enum a9_ho_state { A9_HO_EMPTY, A9_HO_VALID, A9_HO_FAILED, A9_HO_CONSUMED };

enum a9_ho_reason {
	A9_HO_OK,
	A9_HO_MISSING,
	A9_HO_HEADER,
	A9_HO_DIGEST_BAD,
	A9_HO_NOT_CERTIFIED,
	A9_HO_ALREADY_CONSUMED,
	A9_HO_LAYOUT,
	A9_HO_ACKS,
	A9_HO_POOL_CHANGED,
	A9_HO_SRAM_CHANGED,
	A9_HO_WRAPPER,
	A9_HO_UNAVAILABLE,
	A9_HO_REASONS,
};

static const char * const a9_ho_reason_names[A9_HO_REASONS] = {
	"ok", "missing", "header", "digest", "not-certified", "consumed",
	"layout", "acks", "pool-changed", "sram-changed", "wrapper",
	"unavailable",
};

enum a9_ho_path { A9_HO_PATH_NONE, A9_HO_PATH_HELLO, A9_HO_PATH_RESUME };

/* Compact shutdown progress, one bit per completed quiesce step. */
enum a9_ho_step {
	A9_HO_STEP_BEGIN,
	A9_HO_STEP_RATES,
	A9_HO_STEP_AFK_SENT,
	A9_HO_STEP_AFK_ACKED,
	A9_HO_STEP_DISABLED,
	A9_HO_STEP_AP_SENT,
	A9_HO_STEP_AP_ACKED,
	A9_HO_STEP_IOP_SENT,
	A9_HO_STEP_IOP_ACKED,
	A9_HO_STEP_RAM_STABLE,
	A9_HO_STEP_MAILBOX_IDLE,
	A9_HO_STEP_HALTED,
	A9_HO_STEP_DRAINED,
	A9_HO_STEPS,
};

static const char * const a9_ho_step_names[A9_HO_STEPS] = {
	"begin", "rates-zero", "afk-sent", "afk-acked", "endpoint-disable",
	"ap-sent", "ap-ack", "iop-sent", "iop-ack", "ram-stable", "mailbox-idle",
	"halt", "drain",
};

/* Everything a sleep handoff certifies; it never halts or drains. */
#define A9_HO_STEPS_HANDOFF	((1U << (A9_HO_STEP_MAILBOX_IDLE + 1)) - 1)

static const char * const a9_ho_path_names[] = { "none", "hello", "resume" };

/* Indexed by system endpoint 1..4; size 0 means never granted. */
struct a9_ho_sys {
	u32 offset, size, entries, message_size;
};

struct a9_ho_afk {
	u8 ep, reserved;
	u16 tag;
	u32 offset, size;
};

struct a9_ho_record {
	u32 magic, version, size, state;
	u32 generation, quiesce_errno, ap_ack, iop_ack;
	u64 written_ns;
	u8 writer[16], consumer[16];
	char entry[64];
	u64 region_pa, pool_pa;
	u32 region_size, sram_span, pool_size, pool_used;
	u32 ext_offset, ext_size, cpu_control, auto_boot;
	u32 inbox_control, outbox_control, hello_min, hello_max;
	u32 accel_ep, cal_state, nafk, steps;
	u32 failed_step, failed_errno;
	u8 present[32];
	u8 sram_sha256[A9_HO_DIGEST], pool_sha256[A9_HO_DIGEST];
	struct a9_ho_sys sys[A9_SYS_COUNT];
	struct a9_ho_afk afk[A9_HO_MAX_AFK];
	u8 digest[A9_HO_DIGEST];
};

_Static_assert(sizeof(struct a9_ho_record) == 1584, "handoff record ABI");
_Static_assert(sizeof(struct a9_ho_record) <= A9_HO_LOG_OFFSET, "handoff record page");

/* Shutdown transcript: header, then length bytes of newline-separated text.
 * The checksum covers the text, so torn or stale DRAM is never replayed.
 */
struct a9_sl_header {
	u32 magic, version, capacity, length;
	u32 lines, truncated, checksum, mode;
	u64 begin_ns;
	u8 writer[16];
	char entry[64];
};

_Static_assert(sizeof(struct a9_sl_header) == 120, "shutdown log ABI");

#define A9_SL_CAPACITY	(A9_HO_PAGE - A9_HO_LOG_OFFSET - sizeof(struct a9_sl_header))

/* FNV-1a; incremental over appended text. */
static inline u32 a9_sl_hash(u32 hash, const char *text, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++)
		hash = (hash ^ (u8)text[i]) * 16777619U;
	return hash;
}

#define A9_SL_HASH_SEED	2166136261U

/* A replayable previous transcript: complete header, bounded, intact. */
static inline bool a9_sl_valid(const struct a9_sl_header *h, const char *text)
{
	return h->magic == A9_SL_MAGIC && h->version == A9_SL_VERSION &&
	       h->capacity == A9_SL_CAPACITY && h->length <= h->capacity &&
	       !h->entry[sizeof(h->entry) - 1] &&
	       a9_sl_hash(A9_SL_HASH_SEED, text, h->length) == h->checksum;
}

#define A9_HO_DIGESTED	(sizeof(struct a9_ho_record) - A9_HO_DIGEST)

struct a9_ho_expect {
	u64 region_pa, pool_pa;
	u32 region_size, pool_size;	/* allocator capacity, tail page excluded */
};

/* Live driver bookkeeping; the record copy is what this boot validated. */
struct a9_aop_handoff {
	struct a9_ho_record *slot;	/* reserved pool tail, WC mapping */
	struct a9_ho_record rec;	/* validated previous record */
	struct a9_ho_record out;	/* scratch for the outgoing record */
	enum a9_ho_reason reason;
	enum a9_ho_path path;
	bool seen;			/* any magic at probe: no cold reload this boot */
	bool adopt;			/* consumed a valid record */
	bool waking;			/* wake sent, first reply not classified */
	bool sram_valid;
	u8 sram_sha256[A9_HO_DIGEST];	/* resident image fingerprint */
	u8 pool_sha256[A9_HO_DIGEST];	/* quiesce ram-stable digest */
	u8 writer[16];
	u8 afk_used[A9_HO_MAX_AFK];
	u32 generation;			/* resident instance: 0 = loaded this boot */
	u32 idle_cpu, idle_auto_boot, idle_inbox, idle_outbox;
	u32 steps, failed_step, failed_errno;
	struct a9_sl_header *log;	/* tail page + A9_HO_LOG_OFFSET */
	char *log_text, *prev_log;
	size_t prev_len;
	u32 log_hash;
	bool log_on;
	unsigned int sys_adopted, sys_regranted, afk_reused, afk_fresh;
	const char *result;
};

/* Record one quiesce step's outcome; returns @ret unchanged. */
static inline int a9_ho_step(struct a9_aop_handoff *ho, enum a9_ho_step step, int ret)
{
	if (!ret)
		ho->steps |= 1U << step;
	else if (!ho->failed_errno) {
		ho->failed_step = step;
		ho->failed_errno = -ret;
	}
	return ret;
}

static inline bool a9_ho_range(u32 offset, u32 size, u32 limit)
{
	return size && offset <= limit && size <= limit - offset;
}

static inline bool a9_ho_overlap(u32 a, u32 asize, u32 b, u32 bsize)
{
	return a < b + bsize && b < a + asize;
}

static inline bool a9_ho_present(const struct a9_ho_record *r, unsigned int ep)
{
	return ep < 256 && (r->present[ep / 8] >> (ep % 8) & 1);
}

/* Mailbox state required both when writing and when adopting. */
static inline bool a9_ho_wrapper_idle(u32 auto_boot, u32 inbox, u32 outbox,
				      bool outbox_drained)
{
	return auto_boot == 1 && (inbox & A9_HO_MBOX_EMPTY) &&
	       !((inbox | outbox) & A9_HO_MBOX_FAULTS) &&
	       (outbox & A9_HO_OUTBOX_ENABLE) &&
	       (!outbox_drained || (outbox & A9_HO_MBOX_EMPTY));
}

/* Static record checks; @digest is SHA-256 over A9_HO_DIGESTED bytes. */
static inline enum a9_ho_reason a9_ho_check(const struct a9_ho_record *r,
					    const u8 *digest,
					    const struct a9_ho_expect *x)
{
	unsigned int i, j, ep;
	u32 low = 0;

	if (r->magic != A9_HO_MAGIC)
		return A9_HO_MISSING;
	if (r->version != A9_HO_VERSION || r->size != sizeof(*r))
		return A9_HO_HEADER;
	for (i = 0; i < A9_HO_DIGEST; i++)
		low |= r->digest[i] ^ digest[i];
	if (low)
		return A9_HO_DIGEST_BAD;
	if (r->state == A9_HO_CONSUMED)
		return A9_HO_ALREADY_CONSUMED;
	if (r->state != A9_HO_VALID || r->quiesce_errno || r->failed_errno ||
	    r->steps != A9_HO_STEPS_HANDOFF)
		return A9_HO_NOT_CERTIFIED;
	if (r->ap_ack != A9_SYS_AP_QUIESCED || r->iop_ack != A9_SYS_IOP_SLEEP ||
	    !a9_ho_wrapper_idle(r->auto_boot, r->inbox_control,
				r->outbox_control, true))
		return A9_HO_ACKS;
	if (!r->generation || r->generation > A9_HO_GENERATION_MAX ||
	    r->region_pa != x->region_pa || r->region_size != x->region_size ||
	    r->sram_span != A9_HO_SRAM_SPAN || r->sram_span > r->region_size ||
	    r->pool_pa != x->pool_pa || r->pool_size != x->pool_size ||
	    !r->pool_used || r->pool_used > r->pool_size ||
	    !a9_ho_range(r->ext_offset, r->ext_size, r->pool_used) ||
	    r->ext_offset % 0x4000 || r->nafk > A9_HO_MAX_AFK ||
	    r->hello_min > 12 || r->hello_max < 12 ||
	    !a9_ho_present(r, A9_HO_ACCEL_EP) || r->entry[sizeof(r->entry) - 1])
		return A9_HO_LAYOUT;
	if (r->sys[0].size)
		return A9_HO_LAYOUT;
	for (ep = 1; ep < A9_SYS_COUNT; ep++) {
		const struct a9_ho_sys *s = &r->sys[ep];

		if (!s->size)
			continue;
		if (!a9_ho_present(r, ep) || s->offset % A9_SYS_PAGE ||
		    s->size % A9_SYS_PAGE || !a9_ho_range(s->offset, s->size, r->pool_used) ||
		    a9_ho_overlap(s->offset, s->size, r->ext_offset, r->ext_size))
			return A9_HO_LAYOUT;
		for (j = 1; j < ep; j++)
			if (r->sys[j].size && a9_ho_overlap(s->offset, s->size,
							    r->sys[j].offset, r->sys[j].size))
				return A9_HO_LAYOUT;
	}
	for (i = 0; i < r->nafk; i++) {
		const struct a9_ho_afk *a = &r->afk[i];

		if (a->ep < 0x20 || !a9_ho_present(r, a->ep) || !a->tag ||
		    a->offset % 64 || !a9_ho_range(a->offset, a->size, r->pool_used) ||
		    a9_ho_overlap(a->offset, a->size, r->ext_offset, r->ext_size))
			return A9_HO_LAYOUT;
		for (ep = 1; ep < A9_SYS_COUNT; ep++)
			if (r->sys[ep].size && a9_ho_overlap(a->offset, a->size,
							     r->sys[ep].offset, r->sys[ep].size))
				return A9_HO_LAYOUT;
		for (j = 0; j < i; j++)
			if (a9_ho_overlap(a->offset, a->size, r->afk[j].offset, r->afk[j].size) ||
			    (a->ep == r->afk[j].ep && a->tag == r->afk[j].tag))
				return A9_HO_LAYOUT;
	}
	return A9_HO_OK;
}

/* Live measurements against an already statically valid record. */
static inline enum a9_ho_reason a9_ho_check_live(const struct a9_ho_record *r,
						 const u8 *pool_sha, const u8 *sram_sha,
						 u32 auto_boot, u32 inbox, u32 outbox)
{
	unsigned int i;
	u8 pool = 0, sram = 0;

	for (i = 0; i < A9_HO_DIGEST; i++) {
		pool |= r->pool_sha256[i] ^ pool_sha[i];
		sram |= r->sram_sha256[i] ^ sram_sha[i];
	}
	if (pool)
		return A9_HO_POOL_CHANGED;
	if (sram)
		return A9_HO_SRAM_CHANGED;
	/* A self-resumed firmware may have posted its IOP ACK: classify it later. */
	if (!a9_ho_wrapper_idle(auto_boot, inbox, outbox, false))
		return A9_HO_WRAPPER;
	return A9_HO_OK;
}

static inline const struct a9_ho_sys *a9_ho_sys_grant(const struct a9_ho_record *r,
						      unsigned int ep)
{
	return ep && ep < A9_SYS_COUNT && r->sys[ep].size ? &r->sys[ep] : NULL;
}

/* RTBuddySyslog_allocateBuffer b767f8: a nonzero PA maps the existing
 * buffer without a reply; PA 0 reuses the existing one if large enough and
 * replies with its address. Anything else is not the recorded grant.
 */
enum a9_ho_sys_action { A9_HO_SYS_FRESH, A9_HO_SYS_ADOPT, A9_HO_SYS_REGRANT, A9_HO_SYS_MISMATCH };

static inline enum a9_ho_sys_action a9_ho_sys_request(const struct a9_ho_sys *grant,
						      u64 pool_pa, u64 pa, size_t size)
{
	if (!grant)
		return pa ? A9_HO_SYS_MISMATCH : A9_HO_SYS_FRESH;
	if (pa)
		return pa == pool_pa + grant->offset && size == grant->size ?
		       A9_HO_SYS_ADOPT : A9_HO_SYS_MISMATCH;
	return size && size <= grant->size ? A9_HO_SYS_REGRANT : A9_HO_SYS_MISMATCH;
}

/* Reuse each recorded (endpoint, tag, size) AFK region at most once. */
static inline int a9_ho_take_afk(const struct a9_ho_record *r, u8 *used, u8 ep,
				 u16 tag, size_t size, u32 *offset)
{
	unsigned int i;

	for (i = 0; i < r->nafk && i < A9_HO_MAX_AFK; i++) {
		if (used[i] || r->afk[i].ep != ep || r->afk[i].tag != tag ||
		    r->afk[i].size != size)
			continue;
		used[i] = 1;
		*offset = r->afk[i].offset;
		return 0;
	}
	return -ENOENT;
}

#endif
