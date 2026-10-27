/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J71t AOP active transport helpers: old RTBuddy management words, SPU/AFK v8
 * endpoint words, host-initialised shared rings, EPIC command packets and the
 * firmware's tag ("patchbay") table. Pure functions on plain memory so the
 * host harness compiles this header unchanged. MMIO, DMA ordering and
 * locking stay in the kernel caller. See docs/periph/ds-aop-accel.md.
 */
#ifndef _A9_AOP_AFK_H
#define _A9_AOP_AFK_H

#include "a9-aop-protocol.h"

/* Management (endpoint 0): type in payload bits 55:52. */
#define A9_MGMT_HELLO		0x1
#define A9_MGMT_HELLO_REPLY	0x2
#define A9_MGMT_PING		0x3
#define A9_MGMT_PING_ACK	0x4
#define A9_MGMT_STARTEP		0x5
#define A9_MGMT_IOP_POWER_ACK	0x7
#define A9_MGMT_IOP_POWER_ON	0x20
#define A9_MGMT_EPMAP		0x8
#define A9_MGMT_AP_POWER	0xa
#define A9_MGMT_AP_POWER_ACK	0xb

/* SPU/AFK (application endpoints): type in payload bits 55:48. */
#define A9_AFK_INIT		0x80
#define A9_AFK_RX_NOTIFY	0x85
#define A9_AFK_START_ACK	0x86
#define A9_AFK_SHUTDOWN_ACK	0x87
#define A9_AFK_GETBUF		0x89
#define A9_AFK_TX_DESC		0x8a	/* AP->IOP ring, host writes */
#define A9_AFK_RX_DESC		0x8b	/* IOP->AP ring, host reads */
#define A9_AFK_HIST_DESC	0x8c
#define A9_AFK_INIT_ACK		0xa0
#define A9_AFK_GETBUF_ACK	0xa1
#define A9_AFK_DOORBELL		0xa2
#define A9_AFK_START		0xa3
#define A9_AFK_SHUTDOWN		0xa4
#define A9_AFK_HIBERNATE	0xc2
#define A9_AFK_HIBERNATE_ACK	0xc3
#define A9_AFK_VERSION		8
#define A9_AFK_MAX_BUFFERS	16

#define A9_AOP_PA_MASK		0x0000ffffffffffffULL

static inline u8 a9_mgmt_type(u64 payload)
{
	return (payload >> 52) & 0xf;
}

static inline u64 a9_mgmt_word(u8 type, u64 low)
{
	return ((u64)type << 52) | low;
}

/* RTBuddy::createEndpoint: below 0x20 system, from 0x20 RTBuddyEndpointService. */
static inline bool a9_aop_system_endpoint(unsigned int ep)
{
	return ep < 0x20;
}

struct a9_epmap {
	u8 base;
	bool last;
	u32 bitmap;
};

static inline int a9_mgmt_epmap(u64 payload, struct a9_epmap *map)
{
	if (a9_mgmt_type(payload) != A9_MGMT_EPMAP)
		return -EPROTO;
	map->base = (payload >> 32) & 0x3f;
	map->last = (payload >> 51) & 1;
	map->bitmap = (u32)payload;
	return 0;
}

/* iOS: low32 = 1 for the first chunk; later chunks mirror base and last. */
static inline u64 a9_mgmt_epmap_reply(const struct a9_epmap *map)
{
	return a9_mgmt_word(A9_MGMT_EPMAP, ((u64)map->last << 51) |
			    ((u64)map->base << 32) | (map->base == 0));
}

static inline u64 a9_mgmt_startep(u8 ep)
{
	return a9_mgmt_word(A9_MGMT_STARTEP, ((u64)ep << 32) | 2);
}

static inline u64 a9_mgmt_ping(u32 seq, u16 uptime)
{
	return a9_mgmt_word(A9_MGMT_PING, ((u64)uptime << 32) | seq);
}

/* RTBuddy sends seq, expects seq+1; tolerate firmware echoing seq too. */
static inline bool a9_mgmt_ping_ack(u64 payload, u32 seq)
{
	return a9_mgmt_type(payload) == A9_MGMT_PING_ACK &&
	       ((u32)payload == seq || (u32)payload == (u32)(seq + 1));
}

static inline u64 a9_mgmt_ap_power(bool on)
{
	return a9_mgmt_word(A9_MGMT_AP_POWER, on);
}

static inline u8 a9_afk_type(u64 payload)
{
	return payload >> 48;
}

static inline u64 a9_afk_word(u8 type)
{
	return (u64)type << 48;
}

/* 0x89: size in 64-byte blocks at bits 31:16, tag at 15:0. */
static inline int a9_afk_getbuf(u64 payload, u16 *tag, size_t *size)
{
	if (a9_afk_type(payload) != A9_AFK_GETBUF)
		return -EPROTO;
	*tag = payload & 0xffff;
	*size = ((payload >> 16) & 0xffff) * 64;
	return *size ? 0 : -EMSGSIZE;
}

static inline int a9_afk_getbuf_ack(u64 pa, u64 *payload)
{
	if (pa & ~A9_AOP_PA_MASK)
		return -ERANGE;
	*payload = a9_afk_word(A9_AFK_GETBUF_ACK) | pa;
	return 0;
}

/*
 * Shared ring (AppleSPU iop_ringbuffer, block A = 0x40). The AOP initialises
 * the header (AppleSPU constructs the queue with init=0) and the AP only
 * checks it: +0x00 bufsize (= size - 0xc0), +0x04 version word (v7 when the
 * high half is 7), +0x40 rptr (consumer), +0x80 wptr (producer), entries
 * from +0xc0. A v7 entry is +0 magic, +4 payload length (excluding this
 * 16-byte header), +8 channel, +0xc word, then the whole EPIC packet at
 * +0x10; stride = align(16 + length). An entry never wraps: the writer puts
 * it at 0 and leaves a copy of its header at the old wptr, so the reader
 * takes the length from that tail header and reads the entry at 0.
 * Reader fffffff005baba00, writer fffffff005bab540 (docs/periph/aop-sensors.md).
 */
#define A9_RING_BLOCK		0x40
#define A9_RING_HEADER		(3 * A9_RING_BLOCK)
#define A9_RING_V7		0x00070000
#define A9_RING_MAGIC_AOP	0x20504f41	/* "AOP ", host writer */
#define A9_RING_MAGIC_IOP	0x20504f49	/* "IOP " */
#define A9_RING_ENTRY_HEADER	16

/* Kernel callers override these with READ_ONCE/WRITE_ONCE accessors. */
#ifndef A9_RING_LOAD32
#define A9_RING_LOAD32(p) get_unaligned_le32(p)
#define A9_RING_STORE32(v, p) put_unaligned_le32(v, p)
#endif

struct a9_ring {
	u8 *base;	/* ring start: header block 0 */
	u32 size;	/* bytes from the descriptor, header included */
	u32 bufsize;	/* entry area bytes, from the AOP-written header */
	u32 version;
	u32 cursor;	/* RX: local rptr */
};

struct a9_ring_entry {
	u32 magic;
	u32 length;	/* EPIC packet bytes */
	u32 channel;
	u32 word;
	u32 offset;	/* entry-area offset actually read */
};

static inline u32 a9_ring_align(u32 value)
{
	return (value + A9_RING_BLOCK - 1) & ~(u32)(A9_RING_BLOCK - 1);
}

static inline u8 *a9_ring_entry(const struct a9_ring *ring, u32 index)
{
	return ring->base + A9_RING_HEADER + index;
}

static inline u32 a9_ring_rptr(const struct a9_ring *ring)
{
	return A9_RING_LOAD32(ring->base + A9_RING_BLOCK);
}

static inline u32 a9_ring_wptr(const struct a9_ring *ring)
{
	return A9_RING_LOAD32(ring->base + 2 * A9_RING_BLOCK);
}

static inline void a9_ring_set_rptr(struct a9_ring *ring, u32 value)
{
	A9_RING_STORE32(value, ring->base + A9_RING_BLOCK);
}

static inline void a9_ring_set_wptr(struct a9_ring *ring, u32 value)
{
	A9_RING_STORE32(value, ring->base + 2 * A9_RING_BLOCK);
}

static inline bool a9_ring_index_ok(const struct a9_ring *ring, u32 index)
{
	return index < ring->bufsize && !(index % A9_RING_BLOCK);
}

/*
 * Check the AOP-written header of a ring the descriptor just announced;
 * never writes it. The RX cursor starts at the header's rptr.
 */
static inline int a9_ring_attach(struct a9_ring *ring, u8 *base, u32 size)
{
	if (size % A9_RING_BLOCK || size < A9_RING_HEADER + A9_RING_BLOCK)
		return -EINVAL;
	ring->base = base;
	ring->size = size;
	ring->bufsize = A9_RING_LOAD32(base);
	ring->version = A9_RING_LOAD32(base + 4);
	if (ring->bufsize != size - A9_RING_HEADER)
		return -EPROTO;
	if ((ring->version & 0xffff0000) != A9_RING_V7)
		return -EPROTONOSUPPORT;
	ring->cursor = a9_ring_rptr(ring);
	if (!a9_ring_index_ok(ring, ring->cursor) ||
	    !a9_ring_index_ok(ring, a9_ring_wptr(ring)))
		return -ERANGE;
	return 0;
}

/*
 * Read the entry at ring->cursor: copies min(length, cap) EPIC bytes to
 * @packet, fills @entry and the cursor after it in *next (0 at the end).
 * -EAGAIN when empty. Does not write rptr; the caller publishes *next.
 */
static inline int a9_ring_read(const struct a9_ring *ring, u32 wptr,
			       u8 *packet, size_t cap,
			       struct a9_ring_entry *entry, u32 *next)
{
	u32 rptr = ring->cursor, pos, length, stride;
	const u8 *e;

	if (!a9_ring_index_ok(ring, rptr) || !a9_ring_index_ok(ring, wptr))
		return -ERANGE;
	if (rptr == wptr)
		return -EAGAIN;
	pos = rptr + A9_RING_ENTRY_HEADER <= ring->bufsize ? rptr : 0;
	length = A9_RING_LOAD32(a9_ring_entry(ring, pos) + 4);
	stride = a9_ring_align(A9_RING_ENTRY_HEADER + length);
	if (!length || length > ring->bufsize || stride > ring->bufsize)
		return -EMSGSIZE;
	if (pos + stride > ring->bufsize) {
		/* Tail header copy; the entry itself was written at 0. */
		if (pos <= wptr)
			return -EPROTO;
		pos = 0;
	}
	/* The entry must not run past the writer. */
	if (pos < wptr && pos + stride > wptr)
		return -EPROTO;
	e = a9_ring_entry(ring, pos);
	entry->magic = A9_RING_LOAD32(e);
	entry->length = length;
	entry->channel = A9_RING_LOAD32(e + 8);
	entry->word = A9_RING_LOAD32(e + 12);
	entry->offset = pos;
	memcpy(packet, e + A9_RING_ENTRY_HEADER, length < cap ? length : cap);
	*next = pos + stride == ring->bufsize ? 0 : pos + stride;
	return 0;
}

/*
 * Write one EPIC packet using the rptr/wptr just read from the header.
 * On success *new_wptr is the value to publish (after a write barrier);
 * the caller re-reads rptr and rings 0xa2 only if it equals @wptr.
 */
static inline int a9_ring_write(struct a9_ring *ring, u32 rptr, u32 wptr,
				u32 channel, u32 word, const u8 *epic,
				u32 length, u32 *new_wptr)
{
	u32 stride, pos, end;
	u8 *e;

	if (!a9_ring_index_ok(ring, rptr) || !a9_ring_index_ok(ring, wptr))
		return -ERANGE;
	if (!length || length > ring->bufsize)
		return -EMSGSIZE;
	stride = a9_ring_align(A9_RING_ENTRY_HEADER + length);
	pos = wptr;
	if (wptr >= rptr && wptr + stride > ring->bufsize && rptr)
		pos = 0;
	end = pos + stride;
	if ((pos < rptr && end >= rptr) || end > ring->bufsize ||
	    (end == ring->bufsize && !rptr))
		return -ENOSPC;
	e = a9_ring_entry(ring, pos);
	put_unaligned_le32(A9_RING_MAGIC_AOP, e);
	put_unaligned_le32(length, e + 4);
	put_unaligned_le32(channel, e + 8);
	put_unaligned_le32(word, e + 12);
	memcpy(e + A9_RING_ENTRY_HEADER, epic, length);
	memset(e + A9_RING_ENTRY_HEADER + length, 0,
	       stride - A9_RING_ENTRY_HEADER - length);
	if (pos != wptr && wptr + A9_RING_BLOCK <= ring->bufsize)
		memcpy(a9_ring_entry(ring, wptr), e, A9_RING_ENTRY_HEADER);
	*new_wptr = end == ring->bufsize ? 0 : end;
	return 0;
}

static inline u16 a9_epic_sequence(const u8 *packet)
{
	return get_unaligned_le16(packet + 1);
}

/*
 * Property notify: outer(16) + subheader(24) + 00000000 | key | value.
 * Length field = 8 + n, capacity 0xe4 (fffffff005bae168). The outer header
 * (fffffff005baf0fc) leaves +4 zero: the channel goes in the ring entry.
 * Returns the packet length or a negative error.
 */
static inline int a9_epic_property(u8 *packet, size_t cap, u16 sequence,
				   u16 tag, u32 key, const u8 *value, u32 n)
{
	size_t length = A9_SPU_OUTER_SIZE + A9_SPU_SUBHEADER_SIZE + 8 + n;
	u8 *sub = packet + A9_SPU_OUTER_SIZE;

	if (length > cap)
		return -EMSGSIZE;
	memset(packet, 0, length);
	packet[0] = 2;
	put_unaligned_le16(sequence, packet + 1);
	put_unaligned_le32(8 + n, sub);
	sub[4] = 2;
	sub[5] = A9_SPU_NOTIFY;
	put_unaligned_le16(4, sub + 6);
	put_unaligned_le16(tag, sub + 8);
	put_unaligned_le32(0xe4, sub + 20);
	put_unaligned_le32(key, sub + 28);
	memcpy(sub + 32, value, n);
	return length;
}

struct a9_spu_service {
	char name[33];
	u32 service_id;
	u32 flags;
	u32 interface;
	bool has_interface;
};

/* Ready report 0xc0: name32, service id, flags, interface if flags bit 3. */
static inline int a9_spu_service(const struct a9_spu_subpacket *sub,
				 struct a9_spu_service *service)
{
	int ret = a9_spu_announcement(sub, service->name, &service->service_id);

	if (ret)
		return ret;
	service->flags = get_unaligned_le32(sub->payload + 36);
	service->has_interface = service->flags & 8;
	service->interface = 0;
	if (service->has_interface) {
		if (sub->size < 44)
			return -EMSGSIZE;
		service->interface = get_unaligned_le32(sub->payload + 40);
	}
	return 0;
}

/* Firmware image header and tag table (ipad6faop: "uuid" at +0x20). */
#define A9_FW_TAG(a, b, c, d) \
	(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define A9_FW_UUID_MAGIC	A9_FW_TAG('d', 'i', 'u', 'u')
#define A9_FW_HEADER_SIZE	0x50
#define A9_FW_TABLE_MAX		0x1000

/* Validate the whole tag table: [u32 tag][u32 len][len bytes], packed. */
static inline int a9_fw_table(const u8 *fw, size_t size, size_t *offset,
			      size_t *length)
{
	size_t cursor, end;
	u32 value_size;

	if (size < A9_FW_HEADER_SIZE ||
	    get_unaligned_le32(fw + 0x20) != A9_FW_UUID_MAGIC)
		return -EINVAL;
	*offset = get_unaligned_le32(fw + 0x48);
	*length = get_unaligned_le32(fw + 0x4c);
	if (*offset < A9_FW_HEADER_SIZE || *length > A9_FW_TABLE_MAX ||
	    *offset > size || *length > size - *offset)
		return -EMSGSIZE;
	end = *offset + *length;
	for (cursor = *offset; cursor < end; cursor += 8 + value_size) {
		if (end - cursor < 8)
			return -EPROTO;
		value_size = get_unaligned_le32(fw + cursor + 4);
		if (value_size > end - cursor - 8)
			return -EMSGSIZE;
	}
	return 0;
}

/* Find @tag in a table validated by a9_fw_table(). */
static inline int a9_fw_tag(const u8 *fw, size_t offset, size_t length,
			    u32 tag, size_t *value, u32 *value_size)
{
	size_t cursor, end = offset + length;

	for (cursor = offset; cursor + 8 <= end;
	     cursor += 8 + get_unaligned_le32(fw + cursor + 4)) {
		if (get_unaligned_le32(fw + cursor) == tag) {
			*value = cursor + 8;
			*value_size = get_unaligned_le32(fw + cursor + 4);
			return 0;
		}
	}
	return -ENOENT;
}

/* Overwrite an existing tag of exactly @size bytes; absent tags are skipped. */
static inline int a9_fw_patch(u8 *fw, size_t offset, size_t length, u32 tag,
			      const void *data, u32 size)
{
	size_t value;
	u32 value_size;

	if (a9_fw_tag(fw, offset, length, tag, &value, &value_size))
		return -ENOENT;
	if (value_size != size)
		return -EMSGSIZE;
	memcpy(fw + value, data, size);
	return 0;
}

/* The pinned PRNG dispatcher fatals on ECID==0 despite a valid PRNG seed.
 * Validate both requested fields before changing either; preserve ADT bytes
 * (8 little-endian bytes), never manufacture a chip ID or reuse an iOS nonce.
 */
static inline int a9_fw_identity(u8 *fw, size_t size, const u8 *ecid,
				size_t ecid_size, const u8 *nonce)
{
	size_t table, length, ecid_at, nonce_at = 0;
	u32 field_size;
	int ret;

	if (ecid_size != 8 || !get_unaligned_le64(ecid))
		return -EINVAL;
	ret = a9_fw_table(fw, size, &table, &length);
	if (ret)
		return ret;
	ret = a9_fw_tag(fw, table, length, A9_FW_TAG('E', 'C', 'I', 'D'),
			&ecid_at, &field_size);
	if (ret || field_size != 8)
		return ret ?: -EMSGSIZE;
	if (nonce) {
		ret = a9_fw_tag(fw, table, length, A9_FW_TAG('N', 'O', 'N', 'C'),
				&nonce_at, &field_size);
		if (ret || field_size != 8)
			return ret ?: -EMSGSIZE;
	}
	memcpy(fw + ecid_at, ecid, 8);
	if (nonce)
		memcpy(fw + nonce_at, nonce, 8);
	return 0;
}

/*
 * Per-device accel calibration, generated on the host from the private
 * IODeviceTree dump (scripts/periph/aop/make-calibration.py) and staged as
 * a RAM/rootfs-only firmware file. Format: "A9AC", u32 version 1, u32
 * count, then count x {u32 key, u32 length, bytes}. Keys must follow the
 * iOS table order (fffffff006d5e7f8) restricted to unflagged entries.
 */
#define A9_CAL_MAGIC		A9_FW_TAG('C', 'A', '9', 'A')
#define A9_CAL_MAX_SIZE		1024
#define A9_CAL_MAX_BLOB		64

static const u32 a9_cal_order[] = { 0x1a, 0x0b, 0x27, 0x18, 0x19 };

static inline int a9_cal_rank(u32 key)
{
	unsigned int i;

	for (i = 0; i < sizeof(a9_cal_order) / sizeof(a9_cal_order[0]); i++)
		if (a9_cal_order[i] == key)
			return i;
	return -1;
}

struct a9_cal_entry {
	u32 key;
	u32 length;
	const u8 *data;
};

/* Validate the file; fills up to @max entries in order, returns count. */
static inline int a9_cal_parse(const u8 *blob, size_t size,
			       struct a9_cal_entry *entries, unsigned int max)
{
	size_t cursor = 12;
	u32 count, i;
	int rank, last = -1;

	if (size < 12 || size > A9_CAL_MAX_SIZE ||
	    get_unaligned_le32(blob) != A9_CAL_MAGIC ||
	    get_unaligned_le32(blob + 4) != 1)
		return -EINVAL;
	count = get_unaligned_le32(blob + 8);
	if (count > max)
		return -E2BIG;
	for (i = 0; i < count; i++) {
		if (size - cursor < 8)
			return -EMSGSIZE;
		entries[i].key = get_unaligned_le32(blob + cursor);
		entries[i].length = get_unaligned_le32(blob + cursor + 4);
		cursor += 8;
		rank = a9_cal_rank(entries[i].key);
		if (rank <= last || !entries[i].length ||
		    entries[i].length > A9_CAL_MAX_BLOB)
			return -EPROTO;
		if (entries[i].length > size - cursor)
			return -EMSGSIZE;
		last = rank;
		entries[i].data = blob + cursor;
		cursor += entries[i].length;
	}
	return cursor == size ? (int)count : -EPROTO;
}

#endif
