/* SPDX-License-Identifier: GPL-2.0-only */
/* Old A7IOPV2 / SPU v8, NOT the contemporary 96-bit ASC transport. */
#ifndef _A9_AOP_PROTOCOL_H
#define _A9_AOP_PROTOCOL_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define A9_AOP_PAYLOAD_MASK 0x00ffffffffffffffULL
#define A9_SPU_OUTER_SIZE 16
#define A9_SPU_SUBHEADER_SIZE 24
#define A9_SPU_ACCEL_REPORT 0xc2
#define A9_SPU_NOTIFY 0x10
#define A9_SPU_REPLY 0x20
#define A9_SPU_PROPERTY_REPLY 0x84
#define A9_SPU_UNSUPPORTED 0xe00002c7U

struct a9_spu_subpacket {
	u8 category;
	u16 subtype;
	u16 tag;
	u64 timestamp;
	const u8 *payload;
	u32 size;
};

struct a9_aop_motion {
	u16 sequence;
	u32 status;
	s32 axes[3];
};

static inline int a9_aop_pack(u8 endpoint, u64 payload, u64 *wire)
{
	if (payload & ~A9_AOP_PAYLOAD_MASK)
		return -ERANGE;
	*wire = ((u64)endpoint << 56) | payload;
	return 0;
}

static inline u8 a9_aop_endpoint(u64 wire)
{
	return wire >> 56;
}

static inline u64 a9_aop_payload(u64 wire)
{
	return wire & A9_AOP_PAYLOAD_MASK;
}

static inline int a9_aop_hello_reply(u64 wire, u64 *reply)
{
	u64 payload = a9_aop_payload(wire);
	u16 min = payload & 0xffff, max = (payload >> 16) & 0xffff;

	if (a9_aop_endpoint(wire) || ((payload >> 52) & 15) != 1 ||
	    min > 12 || max < 12)
		return -EPROTONOSUPPORT;
	*reply = 0x00200000000c000cULL;
	return 0;
}

/* Descriptor units are 64 bytes. No address translation is implied. */
static inline int a9_spu_queue_descriptor(u64 wire, size_t backing_size,
					  u16 *tag, size_t *offset, size_t *size)
{
	u64 payload = a9_aop_payload(wire);
	u8 type = payload >> 48;

	if (type < 0x8a || type > 0x8c)
		return -EPROTO;
	*tag = payload & 0xffff;
	*size = ((payload >> 16) & 0xffff) * 64;
	*offset = ((payload >> 32) & 0xffff) * 64;
	if (!*size || *offset > backing_size || *size > backing_size - *offset)
		return -EMSGSIZE;
	return 0;
}

/* Returns 1 per subpacket, 0 at end, negative on malformed input. Callers
 * validate the complete outer packet before dispatching any subpacket.
 * Ring/channel envelopes must already have been removed by the transport.
 */
static inline int a9_spu_next(const u8 *packet, size_t length, size_t *cursor,
			      struct a9_spu_subpacket *sub)
{
	const u8 *p;
	u32 size;

	if (length < A9_SPU_OUTER_SIZE || packet[0] != 2 ||
	    *cursor < A9_SPU_OUTER_SIZE || *cursor > length)
		return -EPROTO;
	if (*cursor == length)
		return 0;
	if (length - *cursor < A9_SPU_SUBHEADER_SIZE)
		return -EMSGSIZE;
	p = packet + *cursor;
	size = get_unaligned_le32(p);
	if (p[4] != 2)
		return -EPROTO;
	if (size > length - *cursor - A9_SPU_SUBHEADER_SIZE)
		return -EMSGSIZE;
	sub->category = p[5];
	sub->subtype = get_unaligned_le16(p + 6);
	sub->tag = get_unaligned_le16(p + 8);
	sub->timestamp = get_unaligned_le64(p + 10);
	sub->size = size;
	sub->payload = p + A9_SPU_SUBHEADER_SIZE;
	*cursor += A9_SPU_SUBHEADER_SIZE + size;
	return 1;
}

static inline int a9_spu_motion(const struct a9_spu_subpacket *sub,
				struct a9_aop_motion *motion)
{
	if (sub->category >= 0x10 || sub->subtype != A9_SPU_ACCEL_REPORT)
		return -EPROTO;
	if (sub->size < 22)
		return -EMSGSIZE;
	motion->sequence = get_unaligned_le16(sub->payload);
	motion->status = get_unaligned_le32(sub->payload + 2);
	motion->axes[0] = (s32)get_unaligned_le32(sub->payload + 6);
	motion->axes[1] = (s32)get_unaligned_le32(sub->payload + 10);
	motion->axes[2] = (s32)get_unaligned_le32(sub->payload + 14);
	return 0;
}

static inline int a9_spu_announcement(const struct a9_spu_subpacket *sub,
				      char name[33], u32 *service_id)
{
	if (sub->category >= 0x10 || sub->subtype != 0xc0)
		return -EPROTO;
	if (sub->size < 40)
		return -EMSGSIZE;
	memcpy(name, sub->payload, 32);
	name[32] = 0;
	*service_id = get_unaligned_le32(sub->payload + 32);
	return 0;
}

/* The value below is the property PAYLOAD only. Channel/ring framing,
 * category=notify, subtype=4 and a unique transaction tag are added by the
 * negotiated transport; wait for a correlated IOReturn before proceeding.
 * Stream sequence: batch(0x16), raw(0x5c), report(0); report=0 stops.
 */
static inline void a9_spu_property32(u8 payload[12], u32 key, u32 value)
{
	put_unaligned_le32(0, payload);
	put_unaligned_le32(key, payload + 4);
	put_unaligned_le32(value, payload + 8);
}

/* Complete subtype4 notify subpacket, excluding the outer and queue headers.
 * Timestamp is zero for AP commands; capacity follows the iOS builder (0xe4).
 */
static inline void a9_spu_property_notify(u8 packet[36], u16 tag, u32 key, u32 value)
{
	memset(packet, 0, 36);
	put_unaligned_le32(12, packet);
	packet[4] = 2;
	packet[5] = A9_SPU_NOTIFY;
	put_unaligned_le16(4, packet + 6);
	put_unaligned_le16(tag, packet + 8);
	put_unaligned_le32(0xe4, packet + 20);
	a9_spu_property32(packet + 24, key, value);
}

static inline int a9_spu_property_reply(const struct a9_spu_subpacket *sub, u16 tag)
{
	u32 status;

	/* Firmware 01011d6c sets bit7 on the request subtype: 4 -> 0x84.
	 * iOS bad150 correlates the tag and bae168 reads the leading IOReturn.
	 */
	if (sub->category != A9_SPU_REPLY ||
	    sub->subtype != A9_SPU_PROPERTY_REPLY || sub->tag != tag)
		return -EPROTO;
	if (sub->size < 4)
		return -EMSGSIZE;
	status = get_unaligned_le32(sub->payload);
	if (status == A9_SPU_UNSUPPORTED)
		return -EOPNOTSUPP;
	return status ? -EREMOTEIO : 0;
}

/* Build a stream plan, not traffic. The caller must measure interval units
 * and supported rates, wait for EACH correlated reply, and stop on failure.
 * Disabled plan is only report_interval=0; no raw/batch changes are invented.
 */
static inline int a9_spu_stream_plan(u8 payloads[3][12], bool units_verified,
				     u32 batch, u32 raw, u32 report)
{
	if (!report) {
		a9_spu_property32(payloads[0], 0, 0);
		return 1;
	}
	if (!units_verified)
		return -EOPNOTSUPP;
	a9_spu_property32(payloads[0], 0x16, batch);
	a9_spu_property32(payloads[1], 0x5c, raw);
	a9_spu_property32(payloads[2], 0, report);
	return 3;
}

/* Candidate ARot v1 layout. The caller MUST establish this layout from
 * firmware arithmetic or six-pose evidence before using the result in IIO.
 * No version word is treated as a coefficient, and padding must be zero.
 */
static inline int a9_aop_arot_v1(const u8 *blob, size_t length, s8 matrix[9])
{
	unsigned int row, col, nonzero;

	if (length != 16 || get_unaligned_le32(blob) != 1)
		return -EINVAL;
	for (row = 0; row < 3; row++) {
		nonzero = 0;
		if (blob[4 + row * 4 + 3])
			return -EINVAL;
		for (col = 0; col < 3; col++) {
			s8 value = (s8)blob[4 + row * 4 + col];

			if (value < -1 || value > 1)
				return -EINVAL;
			matrix[row * 3 + col] = value;
			nonzero += value != 0;
		}
		if (nonzero != 1)
			return -EINVAL;
	}
	for (col = 0; col < 3; col++) {
		nonzero = 0;
		for (row = 0; row < 3; row++)
			nonzero += matrix[row * 3 + col] != 0;
		if (nonzero != 1)
			return -EINVAL;
	}
	return 0;
}

#endif
