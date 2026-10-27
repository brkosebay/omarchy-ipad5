/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded snapshot diagnostics; no decisions, MMIO or protocol operations. */
#ifndef _A9_AOP_DIAGNOSTICS_H
#define _A9_AOP_DIAGNOSTICS_H

#include "a9-aop-takeover.h"

#define A9_DIAG_PREFIX_SIZE	0x100
#define A9_DIAG_UUID_OFFSET	0x20
#define A9_DIAG_UUID_SIZE		0x40

struct a9_diag_diff {
	size_t count;
	size_t first; /* size means equal */
};

static inline struct a9_diag_diff a9_diag_compare(const u8 *actual,
						  const u8 *expected, size_t size)
{
	struct a9_diag_diff diff = { .first = size };
	size_t i;

	for (i = 0; i < size; i++) {
		if (actual[i] == expected[i])
			continue;
		if (!diff.count)
			diff.first = i;
		diff.count++;
	}
	return diff;
}

static inline char a9_diag_printable(u8 c)
{
	return c >= 0x20 && c <= 0x7e ? c : '.';
}

struct a9_diag_tag {
	u32 name, size;
	const u8 *data;
};

/* Diagnostic walk only. Even a refused, arbitrary table must be safe to show. */
static inline int a9_diag_next_tag(const u8 *table, size_t size, size_t *cursor,
				   struct a9_diag_tag *tag)
{
	size_t at = *cursor;
	u32 length;

	if (at == size)
		return 0;
	if (at > size || size - at < 8)
		return -EMSGSIZE;
	length = get_unaligned_le32(table + at + 4);
	if (length > size - at - 8)
		return -EMSGSIZE;
	tag->name = get_unaligned_le32(table + at);
	tag->size = length;
	tag->data = table + at + 8;
	*cursor = at + 8 + length;
	return 1;
}

#endif
