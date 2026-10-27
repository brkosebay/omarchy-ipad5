/* SPDX-License-Identifier: GPL-2.0-only */
/* The actual driver policy helpers, compiled unchanged with ASan/UBSan. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-takeover.h"

static u8 *read_image(const char *path)
{
	FILE *f = fopen(path, "rb");
	u8 *image = malloc(A9_ATTACH_MAIN_SIZE);

	assert(f && image);
	assert(fread(image, 1, A9_ATTACH_MAIN_SIZE, f) == A9_ATTACH_MAIN_SIZE);
	assert(fgetc(f) == EOF);
	fclose(f);
	return image;
}

static void fingerprint(struct a9_attach_fingerprint *live, const u8 *image)
{
	memcpy(live->head, image, sizeof(live->head));
	memcpy(live->build, image + A9_ATTACH_BUILD_OFFSET, sizeof(live->build));
	memcpy(live->table, image + A9_ATTACH_TABLE_OFFSET, sizeof(live->table));
}

static void patch(u8 *image, u32 tag, u64 value, u32 n)
{
	u8 bytes[8];

	put_unaligned_le64(value, bytes);
	assert(!a9_fw_patch(image, A9_ATTACH_TABLE_OFFSET, A9_ATTACH_TABLE_SIZE,
			    tag, bytes, n));
}

static enum a9_attach_result check(struct a9_attach_fingerprint *live,
				   const u8 *reference, u64 *pa)
{
	return a9_attach_fingerprint(live, reference, A9_ATTACH_MAIN_SIZE,
				     0x210800000ULL, 8, 0x8003, 1, pa);
}

static void test_mapping(void)
{
	u32 before[A9_ATTACH_REMAP_WORDS] = { 0x10e00000, 2, 0, 0, 0x9b000, 0, 1 };
	u32 after[A9_ATTACH_REMAP_WORDS];
	unsigned int i, bit;

	memcpy(after, before, sizeof(after));
	assert(a9_attach_mapping_matches(before, after, 0x210e00000ULL, 0x9b000));
	for (i = 0; i < A9_ATTACH_REMAP_WORDS; i++) {
		for (bit = 0; bit < 32; bit++) {
			after[i] ^= 1U << bit;
			assert(!a9_attach_mapping_matches(before, after, 0x210e00000ULL, 0x9b000));
			after[i] ^= 1U << bit;
		}
	}
	/* The captured R6 descriptor is unsupported, even if it stays stable. */
	before[4] = after[4] = 0x1ff000;
	assert(!a9_attach_mapping_matches(before, after, 0x210e00000ULL, 0x9b000));
	before[4] = after[4] = 0x9b000;
	assert(!a9_attach_mapping_matches(before, after, 0x210e00000ULL, 0x200000));
	assert(!a9_attach_mapping_matches(before, after, 0x210e01000ULL, 0x9b000));
	assert(!a9_attach_mapping_matches(before, after, ~(u64)0, ~(u64)0));
	before[0] ^= 1; /* an earlier translation changed during the copy */
	assert(!a9_attach_mapping_matches(before, after, 0x210e00000ULL, 0x9b000));
}

static void test_preflight(const char *path)
{
	u8 *reference = read_image(path), *image = read_image(path);
	struct a9_attach_fingerprint live, good;
	u64 pa;
	u32 seed = 17, i, j;
	static const struct { u32 tag, n; u64 value; } tags[] = {
		{ A9_FW_TAG('A', 'O', 'P', 't'), 4, 8 },
		{ A9_FW_TAG('E', 'C', '0', 'p'), 8, 0x880000000ULL },
		{ A9_FW_TAG('W', 'r', 'A', 'd'), 8, 0x210800000ULL },
		{ A9_FW_TAG('n', 'C', 'a', 'l'), 1, 0 },
		{ A9_FW_TAG('S', 'O', 'C', '_'), 4, 0x8003 },
		{ A9_FW_TAG('S', 'O', 'C', 'R'), 4, 1 },
	};

	fingerprint(&live, image);
	assert(check(&live, reference, &pa) == A9_ATTACH_NO_LOADER_TAGS && !pa);
	for (i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
		patch(image, tags[i].tag, tags[i].value, tags[i].n);
	fingerprint(&good, image);
	live = good;
	assert(check(&live, reference, &pa) == A9_ATTACH_OK && pa == 0x880000000ULL);
	/* Live windows must match exactly, not just a UUID/build substring. */
	for (i = 0; i < sizeof(live.head); i++) {
		live = good;
		live.head[i] ^= 1;
		assert(check(&live, reference, &pa) == A9_ATTACH_FOREIGN_IMAGE);
	}
	for (i = 0; i < A9_ATTACH_BUILD_ID_SIZE; i++) {
		live = good;
		live.build[i] ^= 1;
		assert(check(&live, reference, &pa) == A9_ATTACH_FOREIGN_IMAGE);
	}
	/*
	 * The old 64-byte window includes mutable globals, not extra version text.
	 * Firmware copies the randomized loader STKG into build-window+0x34.
	 */
	patch(image, A9_FW_TAG('S', 'T', 'K', 'G'), 0x006a7b8c, 4);
	fingerprint(&live, image);
	assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
	put_unaligned_le32(0x006a7b8c, image + A9_ATTACH_BUILD_OFFSET + A9_ATTACH_GUARD_OFFSET);
	fingerprint(&live, image);
	assert(check(&live, reference, &pa) == A9_ATTACH_OK);
	put_unaligned_le32(17, live.build + 0x38); /* proven mutable cursor */
	assert(check(&live, reference, &pa) == A9_ATTACH_OK);
	live.build[A9_ATTACH_GUARD_OFFSET] ^= 1;
	assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
	patch(image, A9_FW_TAG('S', 'T', 'K', 'G'), 0x00000aff, 4);
	put_unaligned_le32(0x00000aff, image + A9_ATTACH_BUILD_OFFSET + A9_ATTACH_GUARD_OFFSET);
	for (i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
		patch(image, tags[i].tag, tags[i].value ^ 1, tags[i].n);
		fingerprint(&live, image);
		assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
		patch(image, tags[i].tag, tags[i].value, tags[i].n);
	}
	/* No zero, unaligned, out-of-range or overflowing EC0p. */
	{
		const u64 invalid[] = { 0, 0x880000001ULL, 1ULL << 48,
					0xffffffffc000ULL, ~(u64)0 };
		for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
			patch(image, A9_FW_TAG('E', 'C', '0', 'p'), invalid[i], 8);
			fingerprint(&live, image);
			assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
		}
	}
	/* Malformed lengths, truncation, duplicate/missing/reordered tags. */
	live = good;
	put_unaligned_le32(0xffffffff, live.table + 4);
	assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
	live = good;
	memcpy(live.table + 12, live.table, 8);
	assert(check(&live, reference, &pa) == A9_ATTACH_TAG_MISMATCH);
	for (i = 0; i < 20000; i++) {
		live = good;
		for (j = 0; j < sizeof(live.table); j++) {
			seed = seed * 1103515245U + 12345U;
			if (!(seed & 15))
				live.table[j] ^= seed >> 16;
		}
		assert(check(&live, reference, &pa) != A9_ATTACH_FOREIGN_IMAGE);
	}
	assert(a9_attach_contains(0x880000000ULL, 0xc000, 0x880000000ULL, 0xc000));
	assert(!a9_attach_contains(0x880000000ULL, 0x8000, 0x880000000ULL, 0x90a0));
	assert(!a9_attach_contains(0x880000000ULL, 0xc000, 0x87fffffffULL, 1));
	assert(!a9_attach_contains(0xffffffffc000ULL, 0x8000, 0xffffffffc000ULL, 1));
	assert(!a9_attach_contains(0, 0, 0, 1));
	free(reference);
	free(image);
}

static u64 ack(u32 seq)
{
	return a9_mgmt_word(A9_MGMT_PING_ACK, seq);
}

static void ping_ok(struct a9_attach *s, bool plus_one)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		assert(a9_attach_next_ping(s));
		assert(!a9_attach_hello(s, true));
		a9_attach_rx(s, ack(s->seq + plus_one));
	}
	assert(s->stage == A9_ATTACH_PING_OK && s->acks == 2);
}

static void test_ping(void)
{
	u32 seqs[] = { 0, 1, 0xffffffffU };
	unsigned int i;

	for (i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
		u32 seq = seqs[i];

		assert(a9_mgmt_ping_ack(ack(seq), seq));
		assert(a9_mgmt_ping_ack(ack(seq + 1), seq));
		assert(!a9_mgmt_ping_ack(ack(seq + 2), seq));
		assert(!a9_mgmt_ping_ack(a9_mgmt_word(A9_MGMT_IOP_POWER_ACK, seq), seq));
	}
}

static void test_ext(const char *path)
{
	FILE *f = fopen(path, "rb");
	u8 *reference = malloc(A9_ATTACH_EXT_SIZE), *live = malloc(A9_ATTACH_EXT_SIZE);
	unsigned int i;

	assert(f && reference && live);
	assert(fread(reference, 1, A9_ATTACH_EXT_SIZE, f) == A9_ATTACH_EXT_SIZE);
	assert(fgetc(f) == EOF);
	fclose(f);
	memcpy(live, reference, A9_ATTACH_EXT_SIZE);
	assert(a9_attach_ext_matches(live, A9_ATTACH_EXT_SIZE, reference, A9_ATTACH_EXT_SIZE));
	assert(!a9_attach_ext_matches(live, A9_ATTACH_EXT_SIZE - 1, reference, A9_ATTACH_EXT_SIZE));
	assert(!a9_attach_ext_matches(live, A9_ATTACH_EXT_SIZE, reference, A9_ATTACH_EXT_SIZE - 1));
	for (i = 0; i < A9_ATTACH_EXT_SIZE; i += 37) {
		live[i] ^= 1;
		assert(!a9_attach_ext_matches(live, A9_ATTACH_EXT_SIZE, reference, A9_ATTACH_EXT_SIZE));
		live[i] ^= 1;
	}
	free(reference);
	free(live);
}

static void test_state_machine(void)
{
	struct a9_attach s;
	unsigned int i;
	const u64 epmap = 0x0088000100000001ULL;
	const u64 wrong[] = { 0x00100000000c000cULL, 0x0080000100000001ULL,
			      0x0100400000000001ULL, 0x0040000000000009ULL };

	for (i = 0; i < 2; i++) {
		s = (struct a9_attach){ .stage = A9_ATTACH_PING };
		ping_ok(&s, i);
		assert(a9_attach_hello(&s, true) && s.hello_sent);
		assert(!a9_attach_hello(&s, true)); /* never a second HELLO_REPLY */
		a9_attach_rx(&s, epmap);
		assert(s.stage == A9_ATTACH_ADOPTED);
		a9_attach_hello_timeout(&s);
		assert(s.stage == A9_ATTACH_ADOPTED);
		assert(!a9_attach_next_ping(&s) && !a9_attach_hello(&s, true));
	}
	s = (struct a9_attach){ .stage = A9_ATTACH_PING };
	ping_ok(&s, false);
	assert(!a9_attach_hello(&s, false));
	assert(s.result == A9_ATTACH_DISABLED && !s.hello_sent);
	assert(!a9_attach_hello(&s, true)); /* enabling later cannot retry */

	for (i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
		s = (struct a9_attach){ .stage = A9_ATTACH_PING };
		assert(a9_attach_next_ping(&s));
		a9_attach_rx(&s, wrong[i]);
		assert(s.stage == A9_ATTACH_REFUSED && s.result == A9_ATTACH_UNEXPECTED_MGMT);
		assert(!s.hello_sent && !a9_attach_next_ping(&s) && !a9_attach_hello(&s, true));
	}
	/* Two ACK words for a single ping are not two successful exchanges. */
	s = (struct a9_attach){ .stage = A9_ATTACH_PING };
	assert(a9_attach_next_ping(&s) && s.seq == 1);
	a9_attach_rx(&s, ack(2));
	assert(s.acks == 1);
	assert(a9_attach_next_ping(&s) && s.seq == 3);
	a9_attach_rx(&s, ack(2)); /* delayed duplicate from the previous request */
	assert(s.stage == A9_ATTACH_REFUSED);

	s = (struct a9_attach){ .stage = A9_ATTACH_PING };
	for (i = 0; i < A9_ATTACH_PING_TRIES; i++) {
		assert(a9_attach_next_ping(&s));
		a9_attach_ping_timeout(&s);
	}
	assert(s.stage == A9_ATTACH_REFUSED && s.result == A9_ATTACH_NO_PING_ACK);
	assert(!a9_attach_next_ping(&s));
	/* Success, timeout, success is not two consecutive acknowledgments. */
	s = (struct a9_attach){ .stage = A9_ATTACH_PING };
	assert(a9_attach_next_ping(&s));
	a9_attach_rx(&s, ack(s.seq));
	assert(a9_attach_next_ping(&s));
	a9_attach_ping_timeout(&s);
	assert(a9_attach_next_ping(&s));
	a9_attach_rx(&s, ack(s.seq));
	assert(s.acks == 1 && !a9_attach_next_ping(&s));
	assert(s.stage == A9_ATTACH_REFUSED && s.result == A9_ATTACH_NO_PING_ACK);
	/* One lost request followed by two consecutive acks fits the budget. */
	s = (struct a9_attach){ .stage = A9_ATTACH_PING };
	assert(a9_attach_next_ping(&s));
	a9_attach_ping_timeout(&s);
	ping_ok(&s, true);
	assert(s.attempts == 3);
	assert(a9_attach_hello(&s, true));
	a9_attach_hello_timeout(&s);
	assert(s.stage == A9_ATTACH_REFUSED && s.result == A9_ATTACH_HANDSHAKE_OWNED);
	assert(!a9_attach_hello(&s, true));
	/* Anything other than endpoint-zero EPMAP in Phase C is owned. */
	for (i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
		s = (struct a9_attach){ .stage = A9_ATTACH_PING };
		ping_ok(&s, false);
		assert(a9_attach_hello(&s, true));
		a9_attach_rx(&s, wrong[i] == epmap ? 0 : wrong[i]);
		if (a9_aop_endpoint(wrong[i]) || a9_mgmt_type(wrong[i]) != A9_MGMT_EPMAP)
			assert(s.stage == A9_ATTACH_REFUSED && s.result == A9_ATTACH_HANDSHAKE_OWNED);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	test_mapping();
	test_preflight(argv[1]);
	test_ext(argv[2]);
	test_ping();
	test_state_machine();
	puts("PASS: running image/tag preflight, 20000 malformed-table trials, range bounds, seq/seq+1 including wrap, two distinct acks, bounded one-shot attach policy");
	return 0;
}
