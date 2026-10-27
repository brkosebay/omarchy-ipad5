/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-diagnostics.h"

static void test_diffs(void)
{
	u8 actual[0x100] = {}, expected[0x100] = {};
	struct a9_diag_diff diff;
	unsigned int i;

	diff = a9_diag_compare(actual, expected, sizeof(actual));
	assert(!diff.count && diff.first == sizeof(actual));
	actual[0x20] = 0x1b;
	actual[0xff] = 0xff;
	diff = a9_diag_compare(actual, expected, sizeof(actual));
	assert(diff.count == 2 && diff.first == 0x20);
	for (i = 0; i < 256; i++)
		assert(a9_diag_printable(i) == (i >= 32 && i <= 126 ? (char)i : '.'));
}

static void test_tables(void)
{
	u8 bytes[A9_ATTACH_TABLE_SIZE];
	size_t cursor, previous;
	struct a9_diag_tag tag;
	u32 seed = 321;
	unsigned int trial, i;
	int ret;

	memset(bytes, 0, sizeof(bytes));
	put_unaligned_le32(A9_FW_TAG('E', 'C', '0', 'p'), bytes);
	put_unaligned_le32(8, bytes + 4);
	cursor = 0;
	assert(a9_diag_next_tag(bytes, 16, &cursor, &tag) == 1);
	assert(cursor == 16 && tag.size == 8 && tag.data == bytes + 8);
	assert(a9_diag_next_tag(bytes, 16, &cursor, &tag) == 0);
	cursor = 0;
	assert(a9_diag_next_tag(bytes, 15, &cursor, &tag) == -EMSGSIZE);
	put_unaligned_le32(0xffffffff, bytes + 4);
	assert(a9_diag_next_tag(bytes, sizeof(bytes), &cursor, &tag) == -EMSGSIZE);
	cursor = sizeof(bytes) + 1;
	assert(a9_diag_next_tag(bytes, sizeof(bytes), &cursor, &tag) == -EMSGSIZE);
	memset(bytes, 0, sizeof(bytes));
	cursor = 0;
	while (a9_diag_next_tag(bytes, sizeof(bytes), &cursor, &tag) > 0)
		assert(!tag.size && cursor <= sizeof(bytes));
	assert(cursor == 472); /* zero-length TLVs still advance; seven-byte tail */
	for (trial = 0; trial < 20000; trial++) {
		for (i = 0; i < sizeof(bytes); i++) {
			seed = seed * 1103515245U + 12345U;
			bytes[i] = seed >> 16;
		}
		cursor = 0;
		while (true) {
			previous = cursor;
			ret = a9_diag_next_tag(bytes, sizeof(bytes), &cursor, &tag);
			if (ret <= 0)
				break;
			assert(cursor >= previous + 8 && cursor <= sizeof(bytes));
			assert(tag.data + tag.size <= bytes + sizeof(bytes));
		}
	}
}

int main(void)
{
	test_diffs();
	test_tables();
	puts("PASS: frozen-window diffs, bounded printable bytes, refused/malformed TLVs, 20000 diagnostic table trials");
	return 0;
}
