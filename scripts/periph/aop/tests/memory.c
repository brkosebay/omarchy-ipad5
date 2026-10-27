/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "a9-aop-memory.h"

struct bus {
	u8 data[512];
	size_t reads, writes, last;
};

static u32 read_word(void *context, size_t offset)
{
	struct bus *bus = context;

	assert(!(offset & 3) && offset + 4 <= sizeof(bus->data));
	if (bus->reads)
		assert(offset == bus->last + 4);
	bus->last = offset;
	bus->reads++;
	return get_unaligned_le32(bus->data + offset);
}

static void write_word(void *context, size_t offset, u32 value)
{
	struct bus *bus = context;

	assert(!(offset & 3) && offset + 4 <= sizeof(bus->data));
	bus->writes++;
	put_unaligned_le32(value, bus->data + offset);
}

int main(void)
{
	struct bus bus = {};
	u8 copy[512], input[16], image[A9_COLD_SCTLR_OFFSET + 4] = {};
	size_t size, i;

	for (i = 0; i < sizeof(bus.data); i++)
		bus.data[i] = i ^ 0x9d;
	for (size = 0; size <= 479; size++) {
		memset(copy, 0xcc, sizeof(copy));
		bus.reads = 0;
		assert(!a9_aop_copy_from32(&bus, read_word, copy + 1, 4, size, sizeof(bus.data)));
		assert(!memcmp(copy + 1, bus.data + 4, size));
		assert(copy[0] == 0xcc && copy[size + 1] == 0xcc);
		assert(bus.reads == (size + 3) / 4 && !bus.writes);
	}
	/* The unaligned 0x1df table tail reads a whole final u32, never readb. */
	assert(bus.reads == 120 && bus.last == 480);
	bus.reads = 0;
	assert(a9_aop_copy_from32(&bus, read_word, copy, 1, 4, 512) == -ERANGE);
	assert(a9_aop_copy_from32(&bus, read_word, copy, 0, 7, 7) == -ERANGE);
	assert(a9_aop_copy_from32(&bus, read_word, copy, 512, 1, 512) == -ERANGE);
	assert(a9_aop_copy_from32(&bus, read_word, copy, 0, (size_t)-1, 512) == -ERANGE);
	assert(a9_aop_copy_from32(&bus, read_word, copy, (size_t)-4, 3, (size_t)-1) == -ERANGE);
	assert(!bus.reads && !bus.writes);
	for (i = 0; i < sizeof(input); i++)
		input[i] = i * 13;
	assert(!a9_aop_copy_to32(&bus, write_word, input, 4, 16, 512));
	assert(bus.writes == 4 && !bus.reads && !memcmp(bus.data + 4, input, 16));
	assert(a9_aop_copy_to32(&bus, write_word, input, 0, 15, 512) == -ERANGE);
	assert(a9_aop_copy_to32(&bus, write_word, input, 1, 16, 512) == -ERANGE);
	assert(a9_aop_copy_to32(&bus, write_word, input, 508, 16, 512) == -ERANGE);
	assert(bus.writes == 4 && !bus.reads);
	memcpy(copy, bus.data, sizeof(copy));
	struct a9_aop_memory_delta d = a9_aop_memory_compare(bus.data, copy, sizeof(copy));
	assert(!d.words && d.first == (u32)-1);
	copy[7] ^= 0x80;
	copy[508] ^= 1;
	d = a9_aop_memory_compare(bus.data, copy, sizeof(copy));
	assert(d.words == 2 && d.first == 4);
	assert(d.before == get_unaligned_le32(bus.data + 4));
	assert(d.after == get_unaligned_le32(copy + 4));
	/* A span's offsets are relative, and adjacent spans cannot count its word. */
	d = a9_aop_memory_compare(bus.data + 8, copy + 8, 500);
	assert(!d.words);
	d = a9_aop_memory_compare(bus.data + 508, copy + 508, 4);
	assert(d.words == 1 && d.first == 0);
	for (unsigned int test = 0; test < 32; test++)
		assert(a9_aop_cold_test_valid(test) ==
		       (test < 8 || (test >= 12 && test < 16)));
	assert(a9_aop_cold_patch(image, sizeof(image), 8) == -EINVAL);
	put_unaligned_le32(A9_COLD_SCTLR_ORIGINAL, image + A9_COLD_SCTLR_OFFSET);
	assert(!a9_aop_cold_patch(image, sizeof(image), 0));
	assert(!a9_aop_cold_patch(image, sizeof(image), A9_COLD_IOP_IRQS));
	assert(a9_aop_cold_patch(image, sizeof(image), 16) == -EINVAL);
	assert(a9_aop_cold_patch(image, sizeof(image) - 1, 1) == -EILSEQ);
	assert(!a9_aop_cold_patch(image, sizeof(image), A9_COLD_UNCACHED));
	assert(get_unaligned_le32(image + A9_COLD_SCTLR_OFFSET) == A9_COLD_SCTLR_UNCACHED);
	for (i = 0; i < A9_COLD_SCTLR_OFFSET; i++)
		assert(!image[i]);
	assert(a9_aop_cold_patch(image, sizeof(image), A9_COLD_UNCACHED) == -EILSEQ);
	puts("PASS: 32-bit-only aperture reads/writes, 0x1df padded tail, byte copies confined to RAM, alignment/range/overflow refusals");
	return 0;
}
