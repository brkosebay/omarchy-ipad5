/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure readers of a private snapshot, never the live shared ring. */
#ifndef _A9_AOP_RING_H
#define _A9_AOP_RING_H

#include "a9-aop-protocol.h"

#define A9_RR_SIZE	0x4000
#define A9_RR_HEADER	0xc0
#define A9_RR_BUFSIZE	(A9_RR_SIZE - A9_RR_HEADER)
#define A9_RR_MARKER	0x00070006
#define A9_RR_ALIGN	64
#define A9_RR_IOP	0x20504f49
#define A9_RR_AOP	0x20504f41
#define A9_RR_SKIP_RANGES 256

enum a9_rr_range_class {
	A9_RR_CLASS_RAM,
	A9_RR_CLASS_CRASH,
	A9_RR_CLASS_CMA,
	A9_RR_CLASS_RESERVED,
	A9_RR_CLASS_MIXED,
};

enum a9_rr_skip_reason {
	A9_RR_SKIP_NOMAP,
	A9_RR_SKIP_RESERVED,
	A9_RR_SKIP_RESOURCE,
	A9_RR_SKIP_INVALID,
	A9_RR_SKIP_SELF,
	A9_RR_SKIP_FAULT,
	A9_RR_SKIP_REASONS,
};

struct a9_rr_skip_range {
	u64 start;
	u64 end; /* Exclusive. */
	enum a9_rr_skip_reason reason;
};

struct a9_rr_coverage {
	u64 scanned;
	u64 skipped;
	u64 by_reason[A9_RR_SKIP_REASONS];
	u64 omitted_bytes;
	u32 nr_ranges;
	struct a9_rr_skip_range ranges[A9_RR_SKIP_RANGES];
};

/* Caller deduplicates pages; a later fault can revoke a successful page read. */
static inline void a9_rr_skip_page(struct a9_rr_coverage *c, u64 pa,
				   u32 page_size, enum a9_rr_skip_reason reason,
				   bool was_read)
{
	struct a9_rr_skip_range *r;

	if (was_read)
		c->scanned -= page_size;
	c->skipped += page_size;
	c->by_reason[reason] += page_size;
	if (c->nr_ranges) {
		r = &c->ranges[c->nr_ranges - 1];
		if (r->reason == reason && r->end == pa) {
			r->end += page_size;
			return;
		}
	}
	if (c->nr_ranges == A9_RR_SKIP_RANGES) {
		c->omitted_bytes += page_size;
		return;
	}
	r = &c->ranges[c->nr_ranges++];
	r->start = pa;
	r->end = pa + page_size;
	r->reason = reason;
}

/* E1 only: each callback is confined to one physical page, even for a ring. */
static inline int a9_rr_copy_pages(void *ctx, u8 *dst, u64 pa, size_t size,
				  u32 page_size, u64 *failed_page,
				  int (*read)(void *, u8 *, u64, size_t))
{
	size_t chunk;
	int ret;

	if (!page_size || (page_size & (page_size - 1)) || size > ~(u64)0 - pa)
		return -ERANGE;
	while (size) {
		chunk = page_size - (pa & (page_size - 1));
		if (chunk > size)
			chunk = size;
		ret = read(ctx, dst, pa, chunk);
		if (ret) {
			*failed_page = pa & ~((u64)page_size - 1);
			return ret;
		}
		dst += chunk;
		pa += chunk;
		size -= chunk;
	}
	return 0;
}

struct a9_rr_header {
	u32 bufsize;
	u32 marker;
	u32 rptr;
	u32 wptr;
};

struct a9_rr_report {
	struct a9_aop_motion motion;
	u64 timestamp;
	u32 magic;
	u32 length;
	u32 channel;
	u32 reports;
	u16 subtype;
	u8 category;
};

static inline bool a9_rr_index(u32 p)
{
	return p < A9_RR_BUFSIZE && !(p % A9_RR_ALIGN);
}

/* Caller supplies at least 0x84 bytes. Exact signature, not just v7. */
static inline bool a9_rr_header(const u8 *p, struct a9_rr_header *h)
{
	h->bufsize = get_unaligned_le32(p);
	h->marker = get_unaligned_le32(p + 4);
	h->rptr = get_unaligned_le32(p + 0x40);
	h->wptr = get_unaligned_le32(p + 0x80);
	return h->bufsize == A9_RR_BUFSIZE && h->marker == A9_RR_MARKER &&
	       a9_rr_index(h->rptr) && a9_rr_index(h->wptr);
}

/*
 * Read the published interval [cursor, wptr). The writer may leave a tail
 * header and put the actual entry at zero. Length excludes the v7 envelope.
 * Validate ALL subpackets before exposing a report. Never changes @ring.
 * The caller commits @next/@out only on success. No length is trusted until
 * bounded by the ring, the publication boundary and the EPIC packet.
 */
static inline int a9_rr_decode(const u8 *ring, u32 cursor, u32 wptr,
			       u32 *next, struct a9_rr_report *out)
{
	struct a9_rr_report result = {};
	struct a9_spu_subpacket sub;
	const u8 *e, *packet;
	u32 pos, length, stride, magic, count = 0;
	size_t offset;
	int ret;

	if (!a9_rr_index(cursor) || !a9_rr_index(wptr))
		return -ERANGE;
	while (cursor != wptr) {
		if (++count > A9_RR_BUFSIZE / A9_RR_ALIGN)
			return -E2BIG;
		pos = cursor;
		e = ring + A9_RR_HEADER + pos;
		length = get_unaligned_le32(e + 4);
		if (length < A9_SPU_OUTER_SIZE ||
		    length > A9_RR_BUFSIZE - 16)
			return -EMSGSIZE;
		stride = (16 + length + A9_RR_ALIGN - 1) & ~(A9_RR_ALIGN - 1);
		if (stride > A9_RR_BUFSIZE)
			return -EMSGSIZE;
		if (stride > A9_RR_BUFSIZE - pos) {
			if (pos <= wptr)
				return -EPROTO;
			pos = 0;
			e = ring + A9_RR_HEADER;
			if (get_unaligned_le32(e + 4) != length)
				return -EPROTO;
		}
		if (pos < wptr && stride > wptr - pos)
			return -EPROTO;
		magic = get_unaligned_le32(e);
		if (magic != A9_RR_IOP && magic != A9_RR_AOP)
			return -EPROTO;
		if (!result.reports) {
			result.magic = magic;
			result.length = length;
			result.channel = get_unaligned_le32(e + 8);
		}
		packet = e + 16;
		offset = A9_SPU_OUTER_SIZE;
		if (packet[0] != 2)
			return -EPROTO;
		while ((ret = a9_spu_next(packet, length, &offset, &sub)) > 0) {
			if (!result.reports) {
				result.category = sub.category;
				result.subtype = sub.subtype;
			}
			/* Magic direction is unproven; reject non-report subpackets. */
			if (sub.category >= 0x10 ||
			    sub.subtype != A9_SPU_ACCEL_REPORT)
				continue;
			ret = a9_spu_motion(&sub, &result.motion);
			if (ret)
				return ret;
			result.magic = magic;
			result.length = length;
			result.channel = get_unaligned_le32(e + 8);
			result.category = sub.category;
			result.subtype = sub.subtype;
			result.timestamp = sub.timestamp;
			result.reports++;
		}
		if (ret < 0)
			return ret;
		cursor = pos + stride;
		if (cursor == A9_RR_BUFSIZE)
			cursor = 0;
	}
	*next = cursor;
	*out = result;
	return 0;
}

#endif
