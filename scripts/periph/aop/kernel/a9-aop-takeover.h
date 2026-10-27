/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure, host-tested running-AOP preflight and one-shot handshake policy. */
#ifndef _A9_AOP_TAKEOVER_H
#define _A9_AOP_TAKEOVER_H

#include "a9-aop-afk.h"

#define A9_ATTACH_MAIN_SIZE	342336
#define A9_ATTACH_EXT_SIZE	37024
#define A9_ATTACH_REGION_PA	0x210e00000ULL
#define A9_ATTACH_REGION_SIZE	0x9b000
#define A9_ATTACH_REMAP_WORDS	7
#define A9_ATTACH_HEAD_SIZE	0x60
#define A9_ATTACH_BUILD_OFFSET	0x47440
#define A9_ATTACH_BUILD_SIZE	0x40
#define A9_ATTACH_BUILD_ID_SIZE	50 /* 49-byte string plus terminating NUL */
#define A9_ATTACH_GUARD_OFFSET	0x34 /* runtime global, NOT build identity */
#define A9_ATTACH_TABLE_OFFSET	0x389b8
#define A9_ATTACH_TABLE_SIZE	0x1df
#define A9_ATTACH_PAGE_SIZE	0x4000
#define A9_ATTACH_PING_MS		300
#define A9_ATTACH_PING_TRIES	3
#define A9_ATTACH_HELLO_MS	2000
#define A9_ATTACH_HELLO_REPLY	0x00200000000c000cULL

struct a9_attach_fingerprint {
	u8 head[A9_ATTACH_HEAD_SIZE];
	u8 build[A9_ATTACH_BUILD_SIZE];
	u8 table[A9_ATTACH_TABLE_SIZE];
};

/*
 * Conservative software whitelist, not a decoder for unknown remap readback.
 * Only our contiguous cold-loader descriptor is supported. In particular,
 * 0x1ff000 is NOT permission to extend the AP resource or follow another PA.
 * Require the same descriptor around the snapshot before considering identity.
 */
static inline bool a9_attach_mapping_matches(const u32 *before, const u32 *after,
					     u64 region_pa, u64 region_size)
{
	static const u32 expected[A9_ATTACH_REMAP_WORDS] = {
		0x10e00000, 2, 0, 0, A9_ATTACH_REGION_SIZE, 0, 1,
	};

	return region_pa == A9_ATTACH_REGION_PA &&
	       region_size == A9_ATTACH_REGION_SIZE &&
	       !memcmp(before, expected, sizeof(expected)) &&
	       !memcmp(after, expected, sizeof(expected));
}

enum a9_attach_result {
	A9_ATTACH_OK,
	A9_ATTACH_FOREIGN_IMAGE,
	A9_ATTACH_NO_LOADER_TAGS,
	A9_ATTACH_TAG_MISMATCH,
	A9_ATTACH_STALE_EXT,
	A9_ATTACH_UNRESERVED_EXT,
	A9_ATTACH_UNEXPECTED_MGMT,
	A9_ATTACH_NO_PING_ACK,
	A9_ATTACH_DISABLED,
	A9_ATTACH_HANDSHAKE_OWNED,
};

static inline const char *a9_attach_result_name(enum a9_attach_result result)
{
	switch (result) {
	case A9_ATTACH_OK: return "verified";
	case A9_ATTACH_FOREIGN_IMAGE: return "refused-running-foreign-image";
	case A9_ATTACH_NO_LOADER_TAGS: return "refused-running-no-loader-tags";
	case A9_ATTACH_TAG_MISMATCH: return "refused-running-tag-mismatch";
	case A9_ATTACH_STALE_EXT: return "refused-running-stale-ext";
	case A9_ATTACH_UNRESERVED_EXT: return "refused-running-unreserved-ext";
	case A9_ATTACH_UNEXPECTED_MGMT: return "refused-running-unexpected-mgmt";
	case A9_ATTACH_NO_PING_ACK: return "refused-running-no-ping-ack";
	case A9_ATTACH_DISABLED: return "refused-running-no-pending-hello";
	case A9_ATTACH_HANDSHAKE_OWNED: return "refused-running-handshake-owned";
	}
	return "invalid-attach-result";
}

/* Subtraction form avoids wraparound at either end of a physical range. */
static inline bool a9_attach_contains(u64 base, u64 size, u64 pa, u64 length)
{
	return length && size && base <= A9_AOP_PA_MASK &&
	       size - 1 <= A9_AOP_PA_MASK - base && pa >= base &&
	       pa - base < size && length <= size - (pa - base);
}

static inline bool a9_attach_ext_matches(const u8 *live, size_t size,
					 const u8 *reference, size_t reference_size)
{
	return size == A9_ATTACH_EXT_SIZE && reference_size == size &&
	       !memcmp(live, reference, size);
}

/* @reference is the pinned, SHA-256-checked main image, never running RAM. */
static inline enum a9_attach_result
a9_attach_fingerprint(const struct a9_attach_fingerprint *live,
		      const u8 *reference, size_t size, u64 wrapper,
		      u32 target, u32 chip, u32 revision, u64 *ext_pa)
{
	size_t table, length, cursor = 0;
	u32 tag, n;
	u64 value;

	*ext_pa = 0;
	if (size != A9_ATTACH_MAIN_SIZE ||
	    a9_fw_table(reference, size, &table, &length) ||
	    table != A9_ATTACH_TABLE_OFFSET || length != A9_ATTACH_TABLE_SIZE ||
	    memcmp(live->head, reference, sizeof(live->head)) ||
	    memcmp(live->build, reference + A9_ATTACH_BUILD_OFFSET,
		   A9_ATTACH_BUILD_ID_SIZE))
		return A9_ATTACH_FOREIGN_IMAGE;
	if (!memcmp(live->table, reference + table, length))
		return A9_ATTACH_NO_LOADER_TAGS;

	/*
	 * Require the exact pinned table shape, including order and lengths.
	 * This rejects truncated, missing and duplicate tags before reading data.
	 * STKG is FOUR bytes in the artifact (the research table said eight).
	 */
	while (cursor < length) {
		if (length - cursor < 8 ||
		    memcmp(live->table + cursor, reference + table + cursor, 8))
			return A9_ATTACH_TAG_MISMATCH;
		tag = get_unaligned_le32(live->table + cursor);
		n = get_unaligned_le32(live->table + cursor + 4);
		if (n > length - cursor - 8)
			return A9_ATTACH_TAG_MISMATCH;
		cursor += 8;
		/* Every integer read is preceded by the pinned tag-length check. */
		switch (tag) {
		case A9_FW_TAG('A', 'O', 'P', 't'):
			value = target;
			break;
		case A9_FW_TAG('W', 'r', 'A', 'd'):
			value = wrapper;
			break;
		case A9_FW_TAG('M', 'c', 'R', 'A'):
			value = A9_ATTACH_PAGE_SIZE;
			break;
		case A9_FW_TAG('S', 'O', 'C', '_'):
			value = chip;
			break;
		case A9_FW_TAG('S', 'O', 'C', 'R'):
			value = revision;
			break;
		case A9_FW_TAG('n', 'C', 'a', 'l'):
		case A9_FW_TAG('E', 'D', '0', 'p'):
		case A9_FW_TAG('R', 'T', 'T', 'O'):
		case A9_FW_TAG('D', 'E', 'V', 'f'):
			value = 0;
			break;
		case A9_FW_TAG('E', 'C', '0', 'p'):
			*ext_pa = get_unaligned_le64(live->table + cursor);
			goto next;
		case A9_FW_TAG('S', 'T', 'K', 'G'):
			/*
			 * Firmware 0x01000b5c copies the patched tag into +0x47474.
			 * Check consistency instead of comparing to file entropy.
			 */
			if (memcmp(live->build + A9_ATTACH_GUARD_OFFSET,
				   live->table + cursor, n))
				return A9_ATTACH_TAG_MISMATCH;
			goto next;
		case A9_FW_TAG('P', 'R', 'N', 'G'):
		case A9_FW_TAG('N', 'O', 'N', 'C'):
		case A9_FW_TAG('E', 'C', 'I', 'D'):
			goto next; /* loader entropy / per-device identity */
		default:
			if (memcmp(live->table + cursor, reference + table + cursor, n))
				return A9_ATTACH_TAG_MISMATCH;
			goto next;
		}
		if ((n == 1 && live->table[cursor] != value) ||
		    (n == 4 && get_unaligned_le32(live->table + cursor) != value) ||
		    (n == 8 && get_unaligned_le64(live->table + cursor) != value))
			return A9_ATTACH_TAG_MISMATCH;
next:
		cursor += n;
	}
	if (!*ext_pa || (*ext_pa & (A9_ATTACH_PAGE_SIZE - 1)) ||
	    *ext_pa > A9_AOP_PA_MASK ||
	    A9_ATTACH_EXT_SIZE - 1 > A9_AOP_PA_MASK - *ext_pa)
		return A9_ATTACH_TAG_MISMATCH;
	return A9_ATTACH_OK;
}

enum a9_attach_stage {
	A9_ATTACH_IDLE,
	A9_ATTACH_PING,
	A9_ATTACH_PING_OK,
	A9_ATTACH_WAIT_EPMAP,
	A9_ATTACH_ADOPTED,
	A9_ATTACH_REFUSED,
};

struct a9_attach {
	enum a9_attach_stage stage;
	enum a9_attach_result result;
	u32 seq;
	unsigned int attempts, acks;
	bool pending, hello_sent;
};

static inline void a9_attach_refuse(struct a9_attach *s, enum a9_attach_result r)
{
	s->result = r;
	s->stage = A9_ATTACH_REFUSED;
	s->pending = false;
}

/* One outstanding request; a duplicate ack cannot count as two exchanges. */
static inline bool a9_attach_next_ping(struct a9_attach *s)
{
	if (s->stage != A9_ATTACH_PING || s->pending)
		return false;
	if (s->attempts >= A9_ATTACH_PING_TRIES) {
		a9_attach_refuse(s, A9_ATTACH_NO_PING_ACK);
		return false;
	}
	s->attempts++;
	/* Disjoint {seq, seq+1} windows: a delayed duplicate cannot prove life. */
	s->seq = 2 * s->attempts - 1;
	s->pending = true;
	return true;
}

static inline void a9_attach_ping_timeout(struct a9_attach *s)
{
	if (s->stage != A9_ATTACH_PING || !s->pending)
		return;
	s->pending = false;
	s->acks = 0; /* two consecutive successful exchanges */
	if (s->attempts == A9_ATTACH_PING_TRIES)
		a9_attach_refuse(s, A9_ATTACH_NO_PING_ACK);
}

static inline void a9_attach_rx(struct a9_attach *s, u64 wire)
{
	u64 payload = a9_aop_payload(wire);

	if (s->stage == A9_ATTACH_PING || s->stage == A9_ATTACH_PING_OK) {
		if (a9_aop_endpoint(wire) || !s->pending ||
		    !a9_mgmt_ping_ack(payload, s->seq)) {
			a9_attach_refuse(s, A9_ATTACH_UNEXPECTED_MGMT);
			return;
		}
		s->pending = false;
		if (++s->acks == 2)
			s->stage = A9_ATTACH_PING_OK;
	} else if (s->stage == A9_ATTACH_WAIT_EPMAP) {
		if (!a9_aop_endpoint(wire) && a9_mgmt_type(payload) == A9_MGMT_EPMAP)
			s->stage = A9_ATTACH_ADOPTED;
		else
			a9_attach_refuse(s, A9_ATTACH_HANDSHAKE_OWNED);
	}
}

/* True permits exactly ONE fixed HELLO_REPLY, only after the ping proof. */
static inline bool a9_attach_hello(struct a9_attach *s, bool enabled)
{
	if (s->stage != A9_ATTACH_PING_OK || s->hello_sent)
		return false;
	if (!enabled) {
		a9_attach_refuse(s, A9_ATTACH_DISABLED);
		return false;
	}
	s->hello_sent = true;
	s->stage = A9_ATTACH_WAIT_EPMAP;
	return true;
}

static inline void a9_attach_hello_timeout(struct a9_attach *s)
{
	if (s->stage == A9_ATTACH_WAIT_EPMAP)
		a9_attach_refuse(s, A9_ATTACH_HANDSHAKE_OWNED);
}

#endif
