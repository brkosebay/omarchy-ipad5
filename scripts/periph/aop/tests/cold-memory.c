/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual production snapshot callbacks, guarded and width-checked fake MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a9-aop-memory.h"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SHA256_DIGEST_SIZE 32
#define A9_ATTACH_REGION_SIZE 0x9b000
#define A9_ATTACH_BUILD_OFFSET 0x47440
#define A9_ATTACH_GUARD_OFFSET 0x34
#define A9_ATTACH_TABLE_OFFSET 0x389b8
#define AOP_DOMAINS 5
#define AOP_COLD_FRAMES 8
#define AOP_INBOX_CONTROL 0x4008
#define AOP_OUTBOX_CONTROL 0x4020
#define AOP_MBOX_FAULTS 0xc0000
#define EIO 5
#define ENOSPC 28
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
static void log_info(int dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define dev_info log_info
#include "snapshot-types.h"
struct a9_aop {
	int lock, dev, domains[AOP_DOMAINS];
	u8 *region, *autoboot, *cold_before, *cold_after;
	size_t region_size;
	unsigned int cold_frames;
	bool cold_baseline;
	struct a9_aop_cold_frame cold_frame[AOP_COLD_FRAMES];
};
struct apple_pmgr_observation { int unused; };
static u8 sram[A9_ATTACH_REGION_SIZE], before[A9_ATTACH_REGION_SIZE], after[A9_ATTACH_REGION_SIZE];
static unsigned int mmio_reads, guards, next_word;
static bool guard_ok = true;
static u32 fault;
static u64 ktime_get_boottime_ns(void) { return guards; }
static u32 aop_rd(struct a9_aop *aop, u32 offset)
{
	(void)aop;
	assert(guard_ok);
	bool allowed = false;
	for (unsigned int i = 0; i < ARRAY_SIZE(a9_cold_reg_offsets); i++)
		allowed |= offset == a9_cold_reg_offsets[i];
	assert(allowed); /* In particular, no FIFO pop or arbitrary address. */
	mmio_reads++;
	return offset == AOP_INBOX_CONTROL ? fault : 0;
}
static u32 readl(const u8 *p) { assert(guard_ok); return *p; }
static u32 read_word(void *ctx, size_t offset)
{
	assert(ctx == sram && guard_ok && offset == next_word);
	next_word += 4;
	mmio_reads++;
	return get_unaligned_le32(sram + offset);
}
static int a9_aop_fromio32(void *base, size_t cap, u8 *dst, size_t offset, size_t size)
{
	assert(cap == sizeof(sram) && size == cap && !offset);
	next_word = 0;
	return a9_aop_copy_from32(base, read_word, dst, offset, size, cap);
}
static void sha256(const u8 *p, size_t size, u8 *out)
{
	assert(p == after && size == sizeof(sram));
	memset(out, 0x53, SHA256_DIGEST_SIZE);
}
static int apple_pmgr_observe(int *domains, int n, struct apple_pmgr_observation *states,
			      void (*callback)(void *), void *context)
{
	(void)domains; (void)states;
	assert(n == AOP_DOMAINS);
	guards++;
	if (!guard_ok)
		return -EIO;
	callback(context);
	return 0;
}
#include "snapshot-functions.h"
int main(void)
{
	u8 auto_boot = 0;
	struct a9_aop aop = { .region = sram, .region_size = sizeof(sram),
		.autoboot = &auto_boot, .cold_before = before, .cold_after = after };
	memset(sram, 0xa5, sizeof(sram));
	assert(!a9_aop_cold_capture(&aop, "loaded"));
	assert(aop.cold_baseline && !memcmp(before, sram, sizeof(sram)));
	const unsigned int offsets[] = { 0, 0x389c0, 0x47474, 0x53940, 0x9affc };
	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); i++)
		sram[offsets[i]] ^= 0x80;
	assert(!a9_aop_cold_capture(&aop, "released"));
	for (unsigned int i = 0; i < ARRAY_SIZE(a9_cold_spans); i++) {
		struct a9_aop_memory_delta *d = &aop.cold_frame[1].delta[i];
		assert(d->words == 1 && d->first == offsets[i] - a9_cold_spans[i].begin);
	}
	assert(!memcmp(after, sram, sizeof(sram)) && memcmp(before, after, sizeof(sram)));
	unsigned int reads = mmio_reads;
	guard_ok = false;
	assert(a9_aop_cold_capture(&aop, "power-off") == -EIO && mmio_reads == reads);
	guard_ok = true;
	fault = AOP_MBOX_FAULTS;
	assert(a9_aop_cold_capture(&aop, "fault") == -EIO && next_word == sizeof(sram));
	assert(!memcmp(after, sram, sizeof(sram))); /* Last successful copy retained. */
	fault = 0;
	while (aop.cold_frames < AOP_COLD_FRAMES)
		assert(!a9_aop_cold_capture(&aop, "bounded"));
	reads = mmio_reads;
	assert(a9_aop_cold_capture(&aop, "overflow") == -ENOSPC && mmio_reads == reads);
	puts("PASS production SRAM capture: guarded aligned whole-aperture reads, five spans, frozen baseline, fault retention, eight-frame cap, no FIFO pops");
	return 0;
}
