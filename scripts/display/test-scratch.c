// SPDX-License-Identifier: GPL-2.0-only
/* Execute the driver's actual arithmetic gate without a device or kernel. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t u32;
typedef uint64_t u64;
#include <apple-adbe-probe.h>

int main(void)
{
	const u64 frame = 0xc00000;
	u32 word = 0;

	assert(adbe_scratch_word(0x7c95c02a, frame, frame, 0x2400000, 0x2400000, &word));
	assert(word == 0x7d55c02a);
	assert(!adbe_scratch_word(0x7c95c02a, frame, frame, frame, 0x2400000, &word));
	assert(!adbe_scratch_word(0x7c95c02a, frame, frame, 0x2400000, frame, &word));
	assert(!adbe_scratch_word(0x7c95c02a, frame, 64, 0x2400000, 0x2400000, &word));
	assert(!adbe_scratch_word(0x7c95c02a, frame, frame + 1, 0x2400000, 0x2400000, &word));
	assert(!adbe_scratch_word(0x7c95c000, frame, frame, 0x2400000, 0x2400000, &word));
	assert(!adbe_scratch_word(0xff00002a, frame, frame, 0x2400000, 0x2400000, &word));
	assert(!adbe_scratch_word(0x7c95c02a, frame, UINT64_MAX, UINT64_MAX, UINT64_MAX, &word));
	assert(!adbe_scratch_word(0x7c95c02a, 0, frame, 0x2400000, 0x2400000, &word));
	assert(adbe_scratch_word(0x1000002a, frame, frame, frame * 2, frame * 2, &word));
	assert(word == 0x10c0002a);
	puts("PASS: scratch bounds, overlap, alignment, encoding, and address wrap");
	return 0;
}
