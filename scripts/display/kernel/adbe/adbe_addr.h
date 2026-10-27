/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pure arithmetic for the A9 display pipe, shared by the drivers and an
 * offline host test: parameter FIFO word encoding and bus address bounds.
 * No kernel includes; the includer provides bool, u16, u32, u64 and
 * BIT_ULL().
 */
#ifndef ADBE_ADDR_H
#define ADBE_ADDR_H

#define ADBE_BUS_LIMIT		BIT_ULL(32)	/* the pipe's bus is 32-bit */
#define ADBE_ALIGN		64U		/* plane base and stride */

/*
 * Preamble: bit 31, armed bit 30, words that follow [28:16], token [15:0].
 * The pipe loads the token into its frame counter (ADP+0x1034) when it
 * takes the packet (round-2 device log), so it is a frame number.
 */
static inline u32 adbe_pkt_preamble(u32 count, u16 token, bool armed)
{
	return 0x80000000U | (armed ? 0x40000000U : 0) |
	       ((count & 0x1fffU) << 16) | token;
}

/* Entry header: bit 29, one data word in [28:16], register in [15:0]. */
static inline u32 adbe_pkt_header(u32 reg)
{
	return 0x20000000U | (1U << 16) | (reg & 0xffffU);
}

/* Base/stride word: 64-byte aligned value plus a 6-bit tag. */
static inline u32 adbe_tag_word(u32 value, u32 tag)
{
	return (value & ~(ADBE_ALIGN - 1)) | (tag & (ADBE_ALIGN - 1));
}

/*
 * Bus address of [cpu, cpu + len) on the pipe's 32-bit bus, where
 * bus = cpu - offset. Rejects misalignment, underflow and wrap.
 */
static inline bool adbe_cpu_to_bus(u64 cpu, u64 offset, u64 len, u32 *bus)
{
	if (!len || cpu < offset || (cpu & (ADBE_ALIGN - 1)) ||
	    cpu - offset >= ADBE_BUS_LIMIT ||
	    len > ADBE_BUS_LIMIT - (cpu - offset))
		return false;
	*bus = (u32)(cpu - offset);
	return true;
}

/*
 * A scratch frame at @offset after the boot frame, inside the reserved
 * boot VRAM of @reserved bytes, not overlapping the live frame.
 */
static inline bool adbe_scratch_ok(u64 frame, u64 offset, u64 reserved)
{
	return frame && !(offset & (ADBE_ALIGN - 1)) && offset >= frame &&
	       offset <= reserved && frame <= reserved - offset;
}

#endif /* ADBE_ADDR_H */
