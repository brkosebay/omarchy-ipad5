// SPDX-License-Identifier: GPL-2.0-only
/* Execute the drivers' packet encoding and bus bounds without a device. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT_ULL(n) (1ULL << (n))
#include <adbe_addr.h>

int main(void)
{
	const u64 frame = 1536 * 2048 * 4;
	const u64 dram = 0x800000000ULL;
	u32 bus = 0;

	/* Study §13.6: base-only packet, armed, two words, token n. */
	assert(adbe_pkt_preamble(2, 7, true) == 0xc0020007);
	assert(adbe_pkt_header(0x4030) == 0x20014030);
	assert(adbe_pkt_header(0x4038) == 0x20014038);
	assert(adbe_pkt_preamble(4, 0, true) == 0xc0040000);
	/* Upstream ADP "sync": an empty armed preamble. */
	assert(adbe_pkt_preamble(0, 1, true) == 0xc0000001);
	assert(adbe_pkt_preamble(32, 0xffff, true) == 0xc020ffff);
	/* Unarmed: opcode 100. */
	assert(adbe_pkt_preamble(2, 0x2a5, false) == 0x800202a5);

	/* The live word has tag 0; iOS uses 0x2a. */
	assert(adbe_tag_word(0x7d55c000, 0x7c95c000) == 0x7d55c000);
	assert(adbe_tag_word(0x7d55c000, 0x2a) == 0x7d55c02a);
	assert(adbe_tag_word(0x7d55c03f, 0) == 0x7d55c000);

	/* Live framebuffer and a CMA buffer near the top of 2 GiB DRAM. */
	assert(adbe_cpu_to_bus(0x87c95c000ULL, dram, frame, &bus) && bus == 0x7c95c000);
	assert(adbe_cpu_to_bus(0x87f000000ULL, dram, frame, &bus) && bus == 0x7f000000);
	assert(adbe_cpu_to_bus(dram + 0xff400000ULL, dram, frame, &bus));
	assert(!adbe_cpu_to_bus(dram + 0xff400040ULL, dram, frame, &bus));	/* wraps */
	assert(!adbe_cpu_to_bus(0x7ff000000ULL, dram, frame, &bus));		/* below */
	assert(!adbe_cpu_to_bus(0x87c95c020ULL, dram, frame, &bus));		/* align */
	assert(!adbe_cpu_to_bus(0x87c95c000ULL, dram, 0, &bus));
	assert(!adbe_cpu_to_bus(UINT64_MAX & ~63ULL, dram, frame, &bus));

	/* Phase D scratch inside the 36 MiB boot VRAM, after the live frame. */
	assert(adbe_scratch_ok(frame, frame, 0x2400000));
	assert(adbe_scratch_ok(frame, 2 * frame, 0x2400000));
	assert(!adbe_scratch_ok(frame, 2 * frame + 64, 0x2400000));
	assert(!adbe_scratch_ok(frame, frame - 64, 0x2400000));	/* overlap */
	assert(!adbe_scratch_ok(frame, frame + 1, 0x2400000));
	assert(!adbe_scratch_ok(frame, UINT64_MAX & ~63ULL, UINT64_MAX));
	assert(!adbe_scratch_ok(0, frame, 0x2400000));
	puts("PASS: packet words (study 13.6), tag words, bus bounds, scratch bounds");
	return 0;
}
