/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile the actual const snapshot decoder with ASan/UBSan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-ring.h"

static u8 ring[A9_RR_SIZE], saved[A9_RR_SIZE];

static void header(u32 rptr, u32 wptr)
{
	memset(ring, 0, sizeof(ring));
	put_unaligned_le32(A9_RR_BUFSIZE, ring);
	put_unaligned_le32(A9_RR_MARKER, ring + 4);
	put_unaligned_le32(rptr, ring + 0x40);
	put_unaligned_le32(wptr, ring + 0x80);
}

static void entry(u32 pos, u32 magic, u16 seq)
{
	u8 *e = ring + A9_RR_HEADER + pos, *p = e + 16;

	put_unaligned_le32(magic, e);
	put_unaligned_le32(62, e + 4);
	put_unaligned_le32(7, e + 8);
	p[0] = 2;
	put_unaligned_le32(22, p + 16);
	p[20] = 2;
	put_unaligned_le16(0xc2, p + 22);
	put_unaligned_le64(12345 + seq, p + 26);
	put_unaligned_le16(seq, p + 40);
	put_unaligned_le32(0xffff0000, p + 46);
	put_unaligned_le32(32768, p + 50);
	put_unaligned_le32(65536, p + 54);
}

static void test_header(void)
{
	struct a9_rr_header h;

	header(0, 128);
	assert(a9_rr_header(ring, &h));
	assert(h.bufsize == 0x3f40 && h.wptr == 128 && !h.rptr);
	put_unaligned_le32(A9_RR_BUFSIZE, ring + 0x80);
	assert(!a9_rr_header(ring, &h));
	put_unaligned_le32(1, ring + 0x80);
	assert(!a9_rr_header(ring, &h));
	header(0, 0);
	put_unaligned_le32(0x70007, ring + 4);
	assert(!a9_rr_header(ring, &h));
}

static void test_entries(void)
{
	struct a9_rr_report report;
	u32 next = 999;

	header(0, 256);
	entry(0, A9_RR_IOP, 1);
	entry(128, A9_RR_IOP, 2);
	memcpy(saved, ring, sizeof(ring));
	assert(!a9_rr_decode(ring, 0, 256, &next, &report));
	assert(next == 256 && report.reports == 2 && report.motion.sequence == 2);
	assert(report.motion.axes[0] == -65536 && report.motion.axes[1] == 32768);
	assert(report.motion.axes[2] == 65536 && report.timestamp == 12347);
	assert(!memcmp(saved, ring, sizeof(ring))); /* includes shared rptr/wptr */
	assert(!a9_rr_decode(ring, next, 256, &next, &report) && !report.reports);
	/* Later notifications must not relabel the last valid sample. */
	entry(256, A9_RR_AOP, 3);
	ring[A9_RR_HEADER + 256 + 16 + 21] = A9_SPU_NOTIFY;
	assert(!a9_rr_decode(ring, 0, 384, &next, &report));
	assert(report.reports == 2 && report.motion.sequence == 2);
	assert(report.magic == A9_RR_IOP && !report.category && report.subtype == 0xc2);
	/* Both known magics are valid; direction is not proven by the magic. */
	header(0, 128);
	entry(0, A9_RR_AOP, 3);
	assert(!a9_rr_decode(ring, 0, 128, &next, &report) && report.reports == 1);
	/* Host notifications/replies cannot supply accel. */
	ring[A9_RR_HEADER + 16 + 21] = A9_SPU_NOTIFY;
	assert(!a9_rr_decode(ring, 0, 128, &next, &report) && !report.reports);
	/* Tail-header copy, actual entry at zero. */
	header(A9_RR_BUFSIZE - 64, 128);
	entry(0, A9_RR_IOP, 4);
	memcpy(ring + A9_RR_SIZE - 64, ring + A9_RR_HEADER, 16);
	assert(!a9_rr_decode(ring, A9_RR_BUFSIZE - 64, 128, &next, &report));
	assert(next == 128 && report.motion.sequence == 4 && report.reports == 1);
	/* Exact end, no copied header required. */
	header(A9_RR_BUFSIZE - 128, 0);
	entry(A9_RR_BUFSIZE - 128, A9_RR_IOP, 5);
	assert(!a9_rr_decode(ring, A9_RR_BUFSIZE - 128, 0, &next, &report));
	assert(!next && report.motion.sequence == 5);
}

static void test_bad(void)
{
	struct a9_rr_report report, sentinel;
	u32 next = 123;

	header(0, 128);
	entry(0, A9_RR_IOP, 1);
	assert(a9_rr_decode(ring, 1, 128, &next, &report) == -ERANGE);
	assert(a9_rr_decode(ring, 0, A9_RR_BUFSIZE, &next, &report) == -ERANGE);
	assert(a9_rr_decode(ring, 0, 64, &next, &report) == -EPROTO);
	put_unaligned_le32(UINT32_MAX, ring + A9_RR_HEADER + 4);
	assert(a9_rr_decode(ring, 0, 128, &next, &report) == -EMSGSIZE);
	entry(0, A9_RR_IOP, 1);
	put_unaligned_le32(0, ring + A9_RR_HEADER);
	assert(a9_rr_decode(ring, 0, 128, &next, &report) == -EPROTO);
	entry(0, A9_RR_IOP, 1);
	put_unaligned_le32(23, ring + A9_RR_HEADER + 16 + 16);
	assert(a9_rr_decode(ring, 0, 128, &next, &report) == -EMSGSIZE);
	/* A good report followed by malformed data never partially commits. */
	entry(0, A9_RR_IOP, 1);
	put_unaligned_le32(63, ring + A9_RR_HEADER + 4);
	memset(&sentinel, 0x5a, sizeof(sentinel));
	report = sentinel;
	assert(a9_rr_decode(ring, 0, 128, &next, &report) == -EMSGSIZE);
	assert(next == 123 && !memcmp(&sentinel, &report, sizeof(report)));
}

static void test_fuzz(void)
{
	struct a9_rr_report report;
	u32 next, seed = 0x12345678;
	unsigned int i, j;

	for (i = 0; i < 20000; i++) {
		header(0, 128);
		entry(0, A9_RR_IOP, i);
		for (j = 0; j < 8; j++) {
			seed = seed * 1664525 + 1013904223;
			ring[seed % A9_RR_SIZE] ^= seed >> 24;
		}
		memcpy(saved, ring, sizeof(ring));
		a9_rr_decode(ring, (seed % (A9_RR_BUFSIZE / 64)) * 64,
			     ((seed >> 8) % (A9_RR_BUFSIZE / 64)) * 64,
			     &next, &report);
		assert(!memcmp(saved, ring, sizeof(ring)));
	}
}

int main(void)
{
	test_header();
	test_entries();
	test_bad();
	test_fuzz();
	puts("PASS: const ring decoder, wrap/publication bounds, report-category gate, atomic refusal, 20000 malformed trials");
	return 0;
}
