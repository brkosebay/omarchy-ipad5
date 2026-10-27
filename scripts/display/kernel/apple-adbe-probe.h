/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_ADBE_PROBE_H
#define APPLE_ADBE_PROBE_H

/* All sizes are bounded before addition, including the 32-bit bus address. */
static inline bool adbe_scratch_word(u32 base, u64 bytes, u64 offset,
				     u64 mapped, u64 reserved, u32 *word)
{
	u64 address = base & ~0x3fU;

	if ((base & 0x3f) != 0x2a || !bytes || (offset & 0x3f) ||
	    offset < bytes || mapped > reserved || offset > mapped ||
	    bytes > mapped - offset || offset >= (1ULL << 32) ||
	    address + offset + bytes > (1ULL << 32))
		return false;
	*word = (u32)(address + offset) | 0x2a;
	return true;
}

#endif
