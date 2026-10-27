/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the fake RAM/metadata backend, then compile the actual PA capture. */
#define main unused_scan_main
#define a9_aop_ring_pa_own_page reader_target_stub
#include "ring-scan.c"
#undef main
#undef a9_aop_ring_pa_own_page
#include "a9-aop-ring-target.h"

#define __pa_symbol(ptr) storage_pa
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define pr_info(...) do { } while (0)
#define pr_crit(...) do { } while (0)

static struct a9_rt_capture rt_capture;
static u64 storage_pa, change_pa[2];
static int fault_after_wait, online_depth;
static unsigned int waits;
static bool debug, ring_scan, ring_iio;
static unsigned int ring_pa_nr, ring_pa_size_nr;
static unsigned long long ring_pa[A9_AOP_RING_PA_MAX], ring_pa_size[A9_AOP_RING_PA_MAX];

static void get_online_mems(void) { online_depth++; }
static void put_online_mems(void) { assert(online_depth == 1); online_depth--; }
static void dma_rmb(void) { }
static void msleep(unsigned int ms)
{
	assert(ms == 1000 && online_depth == 1);
	waits++;
	now += 1000000000;
	for (unsigned int i = 0; i < 2; i++)
		if (change_pa[i])
			ram[change_pa[i] - FAKE_BASE] ^= 0x40;
	if (fault_after_wait >= 0)
		faulting[fault_after_wait] = true;
}

#include "ring-target-functions.h"

static void target_reset(void)
{
	reset();
	memset(&rt_capture, 0, sizeof(rt_capture));
	memset(ring_pa, 0, sizeof(ring_pa));
	memset(ring_pa_size, 0, sizeof(ring_pa_size));
	storage_pa = FAKE_BASE + FAKE_BYTES + 16 * PAGE_SIZE;
	stack_page.index = FAKE_PAGES + 4;
	change_pa[0] = change_pa[1] = 0;
	fault_after_wait = -1;
	online_depth = 0;
	waits = 0;
	debug = true;
	ring_scan = ring_iio = false;
	ring_pa_nr = ring_pa_size_nr = 1;
	ring_pa[0] = FAKE_BASE + 9 * PAGE_SIZE;
	ring_pa_size[0] = A9_RR_SIZE;
}

static void test_exact_capture(void)
{
	struct a9_rt_target *t = &rt_capture.target[0];
	struct a9_rr_header h;
	u64 pa, bytes_read;

	target_reset();
	pa = ring_pa[0] += 64;
	ring_pa_size[0] = 0x8101; /* Three pages, partial first/last reads. */
	reasons[9] = A9_RR_SKIP_RESERVED;
	reserved_page[10] = true;
	ring_at(pa + 0x200);
	change_pa[0] = pa + 0x200 + 0x80;
	change_pa[1] = pa + ring_pa_size[0] - 1;
	assert(apple_a9_aop_ring_pa_enabled() && apple_a9_aop_ring_mode());
	a9_aop_ring_pa_capture();
	assert(rt_capture.ran && rt_capture.count == 1 && !rt_capture.error);
	assert(!t->error && !t->sample[0].error && !t->sample[1].error);
	assert(attempted_bytes == 2 * ring_pa_size[0]);
	assert(t->sample[0].info.pages == 3 && t->sample[0].info.system_ram_pages == 3);
	assert(t->sample[0].info.reserved_pages == 2 && !t->sample[0].info.nomap_pages);
	assert(!rt_signature(t->sample[0].bytes, t->request.size, 0, &h));
	assert(rt_signature(t->sample[0].bytes, t->request.size, 0x200, &h));
	assert(h.wptr == 128);
	assert(rt_signature(t->sample[1].bytes, t->request.size, 0x200, &h) && h.wptr == 192);
	assert(!rt_signature(t->sample[0].bytes, t->request.size, 1, &h));
	assert(!rt_signature(t->sample[0].bytes, t->request.size, t->request.size - 64, &h));
	assert(t->changed_bytes == 2 && t->changed_lines == 2);
	assert(t->sample[1].ns - t->sample[0].ns >= 1000000000 && waits == 1);
	assert(!online_depth);
	bytes_read = attempted_bytes;
	ram[pa - FAKE_BASE] = 0xff;
	a9_aop_ring_pa_capture();
	assert(attempted_bytes == bytes_read && waits == 1); /* Frozen, no retry. */
}

static void test_lists_and_bounds(void)
{
	struct a9_aop_ring_pa out[A9_AOP_RING_PA_MAX];

	target_reset();
	ring_pa_size_nr = 0;
	assert(apple_a9_aop_ring_mode() && apple_a9_aop_ring_pa_enabled());
	a9_aop_ring_pa_capture();
	assert(rt_capture.error == -EINVAL && !attempted_bytes && !waits);
	debug = false;
	assert(!apple_a9_aop_ring_pa_enabled() && apple_a9_aop_ring_mode());
	target_reset();
	ring_pa_nr = ring_pa_size_nr = A9_AOP_RING_PA_MAX + 1;
	assert(apple_a9_aop_ring_pa_args(out, ARRAY_SIZE(out)) == -EINVAL);
	target_reset();
	ring_pa_nr = ring_pa_size_nr = 4;
	ring_pa[0] = 0;
	ring_pa_size[1] = 0;
	ring_pa[2] = FAKE_BASE;
	ring_pa_size[2] = A9_RT_SIZE_MAX + 1;
	ring_pa[3] = ~(u64)0 - 31;
	ring_pa_size[3] = 64;
	a9_aop_ring_pa_capture();
	for (unsigned int i = 0; i < 4; i++)
		assert(rt_capture.target[i].error == -ERANGE);
	assert(!attempted_bytes && !online_depth);
	target_reset();
	ring_pa_nr = ring_pa_size_nr = 2;
	ring_pa[1] = FAKE_BASE + 16 * PAGE_SIZE;
	ring_pa_size[1] = 256;
	change_pa[0] = ring_pa[1] + 2;
	a9_aop_ring_pa_capture();
	assert(rt_capture.count == 2 && !rt_capture.target[0].changed_bytes);
	assert(rt_capture.target[1].changed_bytes == 1);
	assert(attempted_bytes == 2 * (A9_RR_SIZE + 256) && waits == 1);
}

static void test_rejections(void)
{
	struct a9_rt_sample *s = &rt_capture.target[0].sample[0];

	target_reset();
	reasons[9] = A9_RR_SKIP_NOMAP;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_NOMAP && s->info.nomap_pages == 1);
	assert(s->error == -EACCES && !attempted_bytes);
	target_reset();
	reasons[9] = A9_RR_SKIP_RESOURCE;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_NOT_SYSTEM_RAM && !attempted_bytes);
	target_reset();
	reasons[9] = A9_RR_SKIP_INVALID;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_INVALID && !attempted_bytes);
	target_reset();
	invalid_linear[9] = true;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_INVALID && !attempted_bytes);
	target_reset();
	ring_pa[0] = 0x210e00000ULL;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_NOT_RAM && !attempted_bytes);
	target_reset();
	ring_pa[0] = FAKE_BASE + FAKE_BYTES - 64;
	ring_pa_size[0] = 128;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_NOT_RAM && s->info.pages == 2 && !attempted_bytes);
	target_reset();
	storage_pa = ring_pa[0];
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_SELF && !attempted_bytes);
	target_reset();
	stack_page.index = 9;
	a9_aop_ring_pa_capture();
	assert(s->info.reason == A9_RT_SELF && !attempted_bytes);
}

static void test_fault_quarantine(void)
{
	struct a9_rt_target *t = &rt_capture.target[0];

	target_reset();
	ring_pa_nr = ring_pa_size_nr = 2;
	ring_pa[0] += 64;
	ring_pa_size[0] = 2 * PAGE_SIZE;
	ring_pa[1] = FAKE_BASE + 10 * PAGE_SIZE;
	ring_pa_size[1] = 256;
	faulting[10] = true;
	a9_aop_ring_pa_capture();
	assert(t->sample[0].error == -EFAULT && t->sample[1].error == -EACCES);
	assert(rt_capture.target[1].sample[0].error == -EACCES);
	assert(rt_capture.bad_count == 1 && attempts[10] == 1);
	assert(t->sample[0].info.refused_pa == FAKE_BASE + 10 * PAGE_SIZE);
	assert(!t->changed_bytes && !t->changed_lines);
	target_reset();
	fault_after_wait = 9;
	a9_aop_ring_pa_capture();
	assert(!t->sample[0].error && t->sample[1].error == -EFAULT);
	assert(rt_capture.bad_count == 1 && attempts[9] == 2);
}

int main(void)
{
	ram = malloc(FAKE_BYTES);
	assert(ram);
	test_exact_capture();
	test_lists_and_bounds();
	test_rejections();
	test_fault_quarantine();
	free(ram);
	puts("PASS: production directed PA capture; exact reads, offset signature, 1 s diff, lists/bounds, RAM/no-map/self policy, fault quarantine, freeze");
	return 0;
}
