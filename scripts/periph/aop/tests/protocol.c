/* SPDX-License-Identifier: GPL-2.0-only */
/* Host harness compiles the actual kernel protocol header with endian shims. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-protocol.h"

static void test_transport(void)
{
	u64 word = 0, reply;
	u16 tag;
	size_t offset, size;

	assert(!a9_aop_pack(0xff, A9_AOP_PAYLOAD_MASK, &word));
	assert(word == UINT64_MAX && a9_aop_endpoint(word) == 0xff);
	assert(a9_aop_payload(word) == A9_AOP_PAYLOAD_MASK);
	assert(a9_aop_pack(1, 1ULL << 56, &word) == -ERANGE);
	assert(!a9_aop_hello_reply(0x00100000000c000aULL, &reply));
	assert(reply == 0x00200000000c000cULL);
	assert(a9_aop_hello_reply(0x00100000000b000aULL, &reply) == -EPROTONOSUPPORT);
	assert(a9_aop_hello_reply(0x01100000000c000cULL, &reply) == -EPROTONOSUPPORT);
	word = (0x8aULL << 48) | (3ULL << 32) | (4ULL << 16) | 7;
	assert(!a9_spu_queue_descriptor(word, 448, &tag, &offset, &size));
	assert(tag == 7 && offset == 192 && size == 256);
	assert(a9_spu_queue_descriptor(word, 447, &tag, &offset, &size) == -EMSGSIZE);
	assert(a9_spu_queue_descriptor(word, 1, &tag, &offset, &size) == -EMSGSIZE);
}

static void test_packets(void)
{
	u8 bytes[128] = {}, *p = bytes + 1, plan[3][12], notify[36];
	u8 arot[] = { 1,0,0,0, 255,0,0,0, 0,255,0,0, 0,0,255,0 };
	struct a9_spu_subpacket sub;
	struct a9_aop_motion motion;
	s8 matrix[9];
	size_t cursor;
	unsigned int i;

	/* Deliberately unaligned outer, subheader, timestamp and axes. */
	p[0] = 2;
	put_unaligned_le32(22, p + 16);
	p[20] = 2;
	p[21] = 0;
	put_unaligned_le16(0xc2, p + 22);
	put_unaligned_le16(0x1234, p + 24);
	put_unaligned_le64(0xfedcba9876543210ULL, p + 26);
	put_unaligned_le16(0xabcd, p + 40);
	put_unaligned_le32(0x12345678, p + 42);
	put_unaligned_le32(0xffff0000, p + 46);
	put_unaligned_le32(0, p + 50);
	put_unaligned_le32(65536, p + 54);
	cursor = 16;
	assert(a9_spu_next(p, 62, &cursor, &sub) == 1 && cursor == 62);
	assert(sub.timestamp == 0xfedcba9876543210ULL && sub.tag == 0x1234);
	assert(!a9_spu_motion(&sub, &motion));
	assert(motion.sequence == 0xabcd && motion.status == 0x12345678);
	assert(motion.axes[0] == -65536 && motion.axes[1] == 0 && motion.axes[2] == 65536);
	assert(!a9_spu_next(p, 62, &cursor, &sub));
	for (i = 0; i < 62; i++) {
		cursor = 16;
		assert(a9_spu_next(p, i, &cursor, &sub) < 0 || i == 16);
	}
	put_unaligned_le32(UINT32_MAX, p + 16);
	cursor = 16;
	assert(a9_spu_next(p, 62, &cursor, &sub) == -EMSGSIZE);
	assert(a9_spu_stream_plan(plan, false, 10000, 0, 50000) == -EOPNOTSUPP);
	assert(a9_spu_stream_plan(plan, true, 10000, 0, 50000) == 3);
	assert(get_unaligned_le32(plan[0]) == 0 && get_unaligned_le32(plan[0] + 4) == 0x16);
	assert(get_unaligned_le32(plan[1] + 4) == 0x5c);
	assert(get_unaligned_le32(plan[2] + 4) == 0 && get_unaligned_le32(plan[2] + 8) == 50000);
	assert(a9_spu_stream_plan(plan, false, 0, 0, 0) == 1);
	a9_spu_property_notify(notify, 0x777, 0x18, 3);
	assert(get_unaligned_le32(notify) == 12 && notify[4] == 2 && notify[5] == 0x10);
	assert(get_unaligned_le16(notify + 8) == 0x777 && get_unaligned_le32(notify + 20) == 0xe4);
	sub.category = A9_SPU_REPLY; sub.subtype = 0x84; sub.tag = 0x777;
	sub.payload = plan[0]; sub.size = 4;
	assert(!a9_spu_property_reply(&sub, 0x777));
	assert(a9_spu_property_reply(&sub, 0x778) == -EPROTO);
	sub.subtype = 4;
	assert(a9_spu_property_reply(&sub, 0x777) == -EPROTO);
	sub.subtype = 0x84; sub.size = 3;
	assert(a9_spu_property_reply(&sub, 0x777) == -EMSGSIZE);
	sub.size = 4;
	put_unaligned_le32(0xe0000001, plan[0]);
	assert(a9_spu_property_reply(&sub, 0x777) == -EREMOTEIO);
	assert(!a9_aop_arot_v1(arot, sizeof(arot), matrix));
	assert(matrix[0] == -1 && matrix[4] == -1 && matrix[8] == -1);
	assert(a9_aop_arot_v1(arot, 15, matrix) == -EINVAL);
	arot[7] = 1;
	assert(a9_aop_arot_v1(arot, 16, matrix) == -EINVAL);
	arot[7] = 0; arot[8] = 255; arot[9] = 0;
	assert(a9_aop_arot_v1(arot, 16, matrix) == -EINVAL);
}

static void test_bounds(void)
{
	u8 buffer[256];
	struct a9_spu_subpacket sub;
	size_t length, cursor;
	unsigned int seed = 0x1234, trial, i;

	for (trial = 0; trial < 100000; trial++) {
		for (i = 0; i < sizeof(buffer); i++) {
			seed = seed * 1664525U + 1013904223U;
			buffer[i] = seed >> 24;
		}
		length = trial % sizeof(buffer);
		cursor = trial % 300;
		if (trial & 1)
			buffer[0] = 2;
		while (a9_spu_next(buffer, length, &cursor, &sub) == 1) {
			assert(cursor <= length);
			assert(sub.payload >= buffer && sub.payload + sub.size <= buffer + length);
		}
	}
}

int main(void)
{
	test_transport(); test_packets(); test_bounds();
	puts("PASS: 56-bit transport, hello12, queue bounds, old SPU framing, properties, motion, candidate ARot; 100000 bounded parser trials");
	return 0;
}
