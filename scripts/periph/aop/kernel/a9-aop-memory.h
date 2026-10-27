/* SPDX-License-Identifier: GPL-2.0-only */
/* RTBuddy's firmware aperture contract is aligned 32-bit MMIO, not memcpy. */
#ifndef _A9_AOP_MEMORY_H
#define _A9_AOP_MEMORY_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/* Forced cold only. Identity comes from a per-device RAM firmware file. */
#define A9_COLD_UNCACHED 1U
#define A9_COLD_IOP_IRQS 2U
#define A9_COLD_ECID 4U
#define A9_COLD_NONCE 8U
#define A9_COLD_TEST_MAX 15U
#define A9_COLD_SCTLR_OFFSET 0x29c
#define A9_COLD_SCTLR_ORIGINAL 0xe3011005U
#define A9_COLD_SCTLR_UNCACHED 0xe3011001U
#define A9_COLD_EXT_FENCE_OFFSET 0x4204
#define A9_COLD_EXT_FENCE_ORIGINAL 0x2114
#define A9_COLD_EXT_FENCE_BRANCH 0xe7fe

/* Linked 01072204, after PRNG returned and root+0xc became 0x21, before
 * the method-20 call which sets bit2 at physical 210008004. Park forever;
 * this is a diagnostic boundary, never a resumable firmware boot.
 * Both pristine image hashes must have been checked by the caller.
 */
static inline int a9_aop_cold_ext_fence(u8 *ext, size_t size)
{
	if (size < A9_COLD_EXT_FENCE_OFFSET + 2 ||
	    get_unaligned_le16(ext + A9_COLD_EXT_FENCE_OFFSET) !=
	    A9_COLD_EXT_FENCE_ORIGINAL)
		return -EILSEQ;
	put_unaligned_le16(A9_COLD_EXT_FENCE_BRANCH, ext + A9_COLD_EXT_FENCE_OFFSET);
	return 0;
}

static inline bool a9_aop_cold_test_valid(unsigned int test)
{
	return test <= A9_COLD_TEST_MAX &&
		(!(test & A9_COLD_NONCE) || (test & A9_COLD_ECID));
}

/* Pinned ARM reset stub: MOVW r1,#0x1005 -> #0x1001 before SCTLR ORR.
 * Early startup already clears C; preserve M/I and every other instruction.
 * Call only after both original firmware hashes and forced-stop checks pass.
 */
static inline int a9_aop_cold_patch(u8 *image, size_t size, unsigned int test)
{
	if (!a9_aop_cold_test_valid(test))
		return -EINVAL;
	if (!(test & A9_COLD_UNCACHED))
		return 0;
	if (size < A9_COLD_SCTLR_OFFSET + 4 ||
	    get_unaligned_le32(image + A9_COLD_SCTLR_OFFSET) != A9_COLD_SCTLR_ORIGINAL)
		return -EILSEQ;
	put_unaligned_le32(A9_COLD_SCTLR_UNCACHED, image + A9_COLD_SCTLR_OFFSET);
	return 0;
}

typedef u32 (*a9_aop_read32_fn)(void *context, size_t offset);
typedef void (*a9_aop_write32_fn)(void *context, size_t offset, u32 value);

struct a9_aop_memory_delta {
	u32 words, first, before, after;
};

/* Aligned frozen RAM snapshots only; first is relative to this span. */
static inline struct a9_aop_memory_delta
a9_aop_memory_compare(const u8 *before, const u8 *after, size_t size)
{
	struct a9_aop_memory_delta delta = { .first = (u32)-1 };
	size_t at;
	u32 old, new;

	for (at = 0; at + 4 <= size; at += 4) {
		old = get_unaligned_le32(before + at);
		new = get_unaligned_le32(after + at);
		if (old == new)
			continue;
		if (!delta.words) {
			delta.first = at;
			delta.before = old;
			delta.after = new;
		}
		delta.words++;
	}
	return delta;
}

/*
 * Read whole words, copying only requested bytes to RAM. The padded final
 * read must still fit the allowlisted aperture. In particular the 0x1df-byte
 * tag table needs a 0x1e0-byte bus read, exactly as RTBuddy does.
 */
static inline int a9_aop_copy_from32(void *context, a9_aop_read32_fn read32,
				     u8 *destination, size_t offset, size_t size,
				     size_t aperture_size)
{
	size_t at, i, n, padding = (4 - (size & 3)) & 3;
	u32 value;

	if ((offset & 3) || offset > aperture_size ||
	    size > aperture_size - offset || padding > aperture_size - offset - size)
		return -ERANGE;
	for (at = 0; at < size; at += n) {
		value = read32(context, offset + at);
		n = size - at < 4 ? size - at : 4;
		for (i = 0; i < n; i++)
			destination[at + i] = value >> (8 * i);
	}
	return 0;
}

/* Cold load only: reject partial words instead of read/modify/write. */
static inline int a9_aop_copy_to32(void *context, a9_aop_write32_fn write32,
				   const u8 *source, size_t offset, size_t size,
				   size_t aperture_size)
{
	size_t at;

	if ((offset & 3) || (size & 3) || offset > aperture_size ||
	    size > aperture_size - offset)
		return -ERANGE;
	for (at = 0; at < size; at += 4)
		write32(context, offset + at, get_unaligned_le32(source + at));
	return 0;
}

#endif
