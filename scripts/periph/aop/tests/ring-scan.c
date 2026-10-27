/* SPDX-License-Identifier: GPL-2.0-only */
/* Production E1 functions, fault-injected RAM; no device access. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "a9-aop-ring.h"

#define PAGE_SIZE 16384UL
#define PHYS_PFN(p) ((p) / PAGE_SIZE)
#define ALIGN(v, a) (((v) + (a) - 1) & ~((u64)(a) - 1))
#define PAGE_ALIGN(v) ALIGN(v, PAGE_SIZE)
#define round_down(v, a) ((v) & ~((u64)(a) - 1))
#define RR_HITS 16
#define RR_MAX_BYTES (2ULL << 30)
#define RR_SCAN_NS (45ULL * 1000000000)
#define FAKE_BASE 0x800000000ULL
#define FAKE_BYTES (64UL << 20)
#define FAKE_PAGES (FAKE_BYTES / PAGE_SIZE)
#define current NULL
#define THREAD_SIZE PAGE_SIZE
#define CONFIG_CMA 1
#define IS_ENABLED(v) (v)
#define IORESOURCE_MEM 0x200
#define IORESOURCE_SYSTEM_RAM 0x1000200
#define IORESOURCE_BUSY 0x80000000
#define IORES_DESC_NONE 0
#define IORES_DESC_CRASH_KERNEL 1
#define REGION_DISJOINT 0
#define REGION_INTERSECTS 1

typedef u64 phys_addr_t;
struct resource { u64 start, end; };
struct rr_hit {
	u64 pa;
	enum a9_rr_range_class range_class;
	struct a9_rr_header first, last;
	u8 prefix[0x100];
};
struct a9_ring_reader {
	struct a9_rr_coverage coverage;
	unsigned long *read_pages, *skip_pages;
	unsigned long base_pfn, nr_pages;
	u8 *page, *snapshot;
	u64 walked, scan_start_ns, scan_fault_pa;
	unsigned int nr_hits;
	const char *scan_stop;
	bool scan_reserved;
	struct rr_hit hits[RR_HITS];
};

static u8 *ram;
static int reasons[FAKE_PAGES];
static bool faulting[FAKE_PAGES], interrupted;
static unsigned int attempts[FAKE_PAGES], sleeps;
static u64 attempted_bytes;
static bool reserved_page[FAKE_PAGES], invalid_linear[FAKE_PAGES], crash[FAKE_PAGES];
static unsigned long self_index;
struct page { unsigned long index; };
static struct page pages[FAKE_PAGES], stack_page = { FAKE_PAGES + 4 };
static u8 kernel_stack[THREAD_SIZE];
struct cma { u64 start[2], end[2]; };
static struct cma cma_area;
static u64 now;
static unsigned long read_bits[FAKE_PAGES / 64], skip_bits[FAKE_PAGES / 64];
static struct a9_ring_reader reader;
static u8 page_buffer[PAGE_SIZE], snapshot[A9_RR_SIZE];

static bool test_bit(unsigned long i, const unsigned long *bits)
{
	assert(i < FAKE_PAGES);
	return (bits[i / 64] >> (i % 64)) & 1;
}

static void set_bit(unsigned long i, unsigned long *bits)
{
	assert(i < FAKE_PAGES);
	bits[i / 64] |= 1UL << (i % 64);
}

static bool test_and_set_bit(unsigned long i, unsigned long *bits)
{
	bool old = test_bit(i, bits);

	set_bit(i, bits);
	return old;
}

static bool test_and_clear_bit(unsigned long i, unsigned long *bits)
{
	bool old = test_bit(i, bits);

	bits[i / 64] &= ~(1UL << (i % 64));
	return old;
}

static bool pfn_valid(unsigned long pfn)
{
	unsigned long i = pfn - PHYS_PFN(FAKE_BASE);

	return i < FAKE_PAGES && reasons[i] != A9_RR_SKIP_INVALID;
}

static bool a9_aop_ring_pa_own_page(u64 pa) { (void)pa; return false; }

static bool pfn_is_map_memory(unsigned long pfn)
{
	unsigned long i = pfn - PHYS_PFN(FAKE_BASE);

	return i < FAKE_PAGES && reasons[i] != A9_RR_SKIP_NOMAP;
}

static bool memblock_is_region_reserved(u64 pa, size_t size)
{
	assert(size == PAGE_SIZE);
	return reasons[(pa - FAKE_BASE) / PAGE_SIZE] == A9_RR_SKIP_RESERVED;
}

static bool memblock_is_region_memory(u64 pa, size_t size)
{
	return pa >= FAKE_BASE && pa < FAKE_BASE + FAKE_BYTES &&
		size <= FAKE_BASE + FAKE_BYTES - pa;
}

static struct page *pfn_to_page(unsigned long pfn)
{
	assert(pfn_valid(pfn));
	return &pages[pfn - PHYS_PFN(FAKE_BASE)];
}

static bool PageReserved(struct page *page) { return reserved_page[page->index]; }

static int region_intersects(u64 pa, size_t size, unsigned long flags, unsigned long desc)
{
	size_t i = (pa - FAKE_BASE) / PAGE_SIZE;

	assert(i < FAKE_PAGES && size == PAGE_SIZE);
	if (desc == IORES_DESC_CRASH_KERNEL) {
		assert(flags == IORESOURCE_MEM);
		return crash[i] ? REGION_INTERSECTS : REGION_DISJOINT;
	}
	assert(desc == IORES_DESC_NONE && flags == (IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY));
	return reasons[i] == A9_RR_SKIP_RESOURCE ? REGION_DISJOINT : REGION_INTERSECTS;
}

static bool cma_intersects(struct cma *cma, unsigned long start, unsigned long end)
{
	for (unsigned int i = 0; i < 2; i++)
		if (cma->end[i] && start < cma->end[i] && end >= cma->start[i])
			return true;
	return false;
}

static int cma_for_each_area(int (*cb)(struct cma *, void *), void *data)
{
	return cb(&cma_area, data);
}

static u64 virt_to_phys(const void *ptr)
{
	return FAKE_BASE + (ptr == &reader ? self_index : FAKE_PAGES + 2) * PAGE_SIZE;
}

static bool virt_addr_valid(const u8 *ptr) { return !invalid_linear[(ptr - ram) / PAGE_SIZE]; }
static void *task_stack_page(void *unused) { (void)unused; return kernel_stack; }
static bool is_vmalloc_addr(const void *ptr) { (void)ptr; return false; }
static struct page *vmalloc_to_page(const void *ptr) { (void)ptr; return &stack_page; }
static struct page *virt_to_page(const void *ptr) { return vmalloc_to_page(ptr); }
static u64 page_to_phys(struct page *page) { return FAKE_BASE + page->index * PAGE_SIZE; }
static size_t bitmap_size(unsigned long bits) { return (bits + 63) / 64 * sizeof(unsigned long); }

static const u8 *phys_to_virt(u64 pa)
{
	assert(pa >= FAKE_BASE && pa < FAKE_BASE + FAKE_BYTES);
	return ram + pa - FAKE_BASE;
}

static int copy_from_kernel_nofault(u8 *dst, const u8 *src, size_t size)
{
	size_t offset = src - ram, i = offset / PAGE_SIZE;

	assert(offset < FAKE_BYTES && size <= FAKE_BYTES - offset);
	assert(offset % PAGE_SIZE + size <= PAGE_SIZE); /* No cross-page probe. */
	attempts[i]++;
	attempted_bytes += size;
	if (faulting[i])
		return -EFAULT;
	memcpy(dst, src, size);
	return 0;
}

static bool fatal_signal_pending(void *unused)
{
	(void)unused;
	return interrupted;
}

static u64 ktime_get_boottime_ns(void) { return now; }
static void cond_resched(void) { }
static void usleep_range(unsigned long min, unsigned long max)
{
	assert(min == 4000 && max == 5000);
	sleeps++;
	now += min * 1000;
}
static void rr_page_state(struct a9_ring_reader *rr, struct rr_hit *hit)
{
	(void)rr;
	(void)hit;
}

#include "ring-scan-functions.h"

static void reset(void)
{
	memset(&reader, 0, sizeof(reader));
	memset(read_bits, 0, sizeof(read_bits));
	memset(skip_bits, 0, sizeof(skip_bits));
	memset(faulting, 0, sizeof(faulting));
	memset(attempts, 0, sizeof(attempts));
	memset(reserved_page, 0, sizeof(reserved_page));
	memset(invalid_linear, 0, sizeof(invalid_linear));
	memset(crash, 0, sizeof(crash));
	memset(&cma_area, 0, sizeof(cma_area));
	self_index = FAKE_PAGES + 2;
	memset(ram, 0, FAKE_BYTES);
	for (size_t i = 0; i < FAKE_PAGES; i++) {
		reasons[i] = -1;
		pages[i].index = i;
	}
	reader.base_pfn = PHYS_PFN(FAKE_BASE);
	reader.nr_pages = FAKE_PAGES;
	reader.read_pages = read_bits;
	reader.skip_pages = skip_bits;
	reader.page = page_buffer;
	reader.snapshot = snapshot;
	interrupted = false;
	now = 0;
	sleeps = 0;
	attempted_bytes = 0;
}

static void ring_at(u64 pa)
{
	u8 *p = ram + pa - FAKE_BASE;

	put_unaligned_le32(A9_RR_BUFSIZE, p);
	put_unaligned_le32(A9_RR_MARKER, p + 4);
	put_unaligned_le32(128, p + 0x80);
}

static void test_reported_fault(void)
{
	struct resource res = { FAKE_BASE, FAKE_BASE + FAKE_BYTES - 1 };
	u64 fault_pa = 0x8035f0000ULL;
	size_t index = (fault_pa - FAKE_BASE) / PAGE_SIZE;

	reset();
	faulting[index] = true;
	reasons[2] = A9_RR_SKIP_RESERVED;
	reasons[3] = A9_RR_SKIP_NOMAP;
	ring_at(FAKE_BASE + FAKE_BYTES - A9_RR_SIZE);
	assert(!rr_scan_range(&res, &reader));
	assert(reader.walked == FAKE_BYTES && reader.nr_hits == 1);
	assert(reader.hits[0].pa > fault_pa && reader.scan_fault_pa == fault_pa);
	assert(reader.coverage.scanned == FAKE_BYTES - 3 * PAGE_SIZE);
	assert(reader.coverage.skipped == 3 * PAGE_SIZE);
	assert(reader.coverage.by_reason[A9_RR_SKIP_FAULT] == PAGE_SIZE);
	assert(attempts[index] == 1 && !attempts[2] && !attempts[3]);
	assert(sleeps == FAKE_BYTES / (256 * 1024));
}

static void test_cross_page_and_revocation(void)
{
	struct resource res = { FAKE_BASE, FAKE_BASE + 8 * PAGE_SIZE - 1 };
	unsigned int before;

	reset();
	ring_at(FAKE_BASE + 64); /* The complete ring needs a second physical page. */
	faulting[1] = true;
	ring_at(FAKE_BASE + 4 * PAGE_SIZE);
	assert(!rr_scan_range(&res, &reader));
	assert(reader.nr_hits == 1 && reader.hits[0].pa == FAKE_BASE + 4 * PAGE_SIZE);
	assert(reader.coverage.scanned == 7 * PAGE_SIZE && attempts[1] == 1);
	/* Watch-time fault of a previously read page moves it to skipped once. */
	faulting[4] = true;
	before = attempts[4];
	assert(rr_scan_copy(&reader, snapshot, FAKE_BASE + 4 * PAGE_SIZE, A9_RR_SIZE) == -EFAULT);
	assert(reader.coverage.scanned == 6 * PAGE_SIZE);
	assert(reader.coverage.skipped == 2 * PAGE_SIZE);
	assert(rr_scan_copy(&reader, snapshot, FAKE_BASE + 4 * PAGE_SIZE, A9_RR_SIZE) == -EACCES);
	assert(attempts[4] == before + 1 && reader.coverage.skipped == 2 * PAGE_SIZE);
}

static void reserved_fixture(void)
{
	reset();
	for (unsigned int i = 0; i < 3; i++) {
		reasons[i] = A9_RR_SKIP_RESERVED;
		ring_at(FAKE_BASE + i * PAGE_SIZE);
	}
	crash[0] = true;
	cma_area.start[0] = FAKE_BASE + PAGE_SIZE;
	cma_area.end[0] = FAKE_BASE + 2 * PAGE_SIZE;
	cma_area.start[1] = FAKE_BASE + 15 * PAGE_SIZE;
	cma_area.end[1] = FAKE_BASE + 16 * PAGE_SIZE;
	reserved_page[3] = true; /* PageReserved even without a memblock reservation. */
	ring_at(FAKE_BASE + 3 * PAGE_SIZE);
	reasons[4] = A9_RR_SKIP_NOMAP;
	crash[4] = true; /* Classification must never authorize a no-map read. */
	reasons[5] = A9_RR_SKIP_RESOURCE;
	reserved_page[5] = true;
	reasons[6] = A9_RR_SKIP_INVALID;
	invalid_linear[7] = true;
	self_index = 8;
	reasons[8] = A9_RR_SKIP_RESERVED;
	ring_at(FAKE_BASE + 9 * PAGE_SIZE);
	ring_at(FAKE_BASE + 10 * PAGE_SIZE + 64);
	reasons[11] = A9_RR_SKIP_RESERVED;
	crash[11] = true;
}

static void test_reserved_opt_in(void)
{
	struct resource res = { FAKE_BASE, FAKE_BASE + 12 * PAGE_SIZE - 1 };
	static const enum a9_rr_range_class expected[] = {
		A9_RR_CLASS_CRASH, A9_RR_CLASS_CMA, A9_RR_CLASS_RESERVED,
		A9_RR_CLASS_RESERVED, A9_RR_CLASS_RAM, A9_RR_CLASS_MIXED,
	};

	reserved_fixture();
	assert(!rr_scan_range(&res, &reader));
	assert(reader.nr_hits == 1 && reader.hits[0].pa == FAKE_BASE + 9 * PAGE_SIZE);
	assert(reader.coverage.by_reason[A9_RR_SKIP_RESERVED] > 0);
	for (unsigned int i = 0; i < 4; i++)
		assert(!attempts[i]);
	reserved_fixture();
	reader.scan_reserved = true;
	assert(!rr_scan_range(&res, &reader));
	assert(reader.nr_hits == sizeof(expected) / sizeof(expected[0]));
	for (unsigned int i = 0; i < reader.nr_hits; i++)
		assert(reader.hits[i].range_class == expected[i]);
	for (unsigned int i = 4; i <= 8; i++)
		assert(!attempts[i]); /* no-map, non-RAM resource, invalid PFN/map, self */
	assert(reader.coverage.scanned == 7 * PAGE_SIZE);
	assert(reader.coverage.skipped == 5 * PAGE_SIZE);
	assert(!reader.coverage.by_reason[A9_RR_SKIP_RESERVED]);
	assert(rr_hit_class(FAKE_BASE + 15 * PAGE_SIZE) == A9_RR_CLASS_CMA);
	assert(rr_scan_copy(&reader, snapshot, 0x210e00000ULL, A9_RR_SIZE) == -EACCES);
	/* Fault quarantine is independent of the reserved-page admission flag. */
	faulting[0] = true;
	assert(rr_scan_copy(&reader, snapshot, FAKE_BASE, A9_RR_SIZE) == -EFAULT);
	assert(rr_scan_copy(&reader, snapshot, FAKE_BASE, A9_RR_SIZE) == -EACCES);
	assert(reader.coverage.scanned == 6 * PAGE_SIZE && reader.coverage.skipped == 6 * PAGE_SIZE);
}

static void test_limits(void)
{
	struct resource res = { FAKE_BASE, FAKE_BASE + FAKE_BYTES - 1 };
	struct a9_rr_coverage *c = &reader.coverage;

	reset();
	reader.walked = RR_MAX_BYTES;
	assert(rr_scan_range(&res, &reader) == 1 && !strcmp(reader.scan_stop, "budget"));
	assert(!attempts[0]);
	reset();
	now = RR_SCAN_NS;
	assert(rr_scan_range(&res, &reader) == 1 && !attempts[0]);
	reset();
	interrupted = true;
	assert(rr_scan_range(&res, &reader) == -EINTR && !attempts[0]);
	reset();
	for (size_t i = 0; i < RR_HITS; i++)
		ring_at(FAKE_BASE + i * PAGE_SIZE);
	assert(rr_scan_range(&res, &reader) == 1 && reader.nr_hits == RR_HITS);
	assert(!strcmp(reader.scan_stop, "hit-limit"));
	reset();
	/* Coalesce adjacent skips, disclose fragmentation beyond the storage cap. */
	a9_rr_skip_page(c, FAKE_BASE, PAGE_SIZE, A9_RR_SKIP_RESERVED, false);
	a9_rr_skip_page(c, FAKE_BASE + PAGE_SIZE, PAGE_SIZE, A9_RR_SKIP_RESERVED, false);
	assert(c->nr_ranges == 1 && c->ranges[0].end == FAKE_BASE + 2 * PAGE_SIZE);
	for (size_t i = 1; i < A9_RR_SKIP_RANGES + 10; i++)
		a9_rr_skip_page(c, FAKE_BASE + i * 3 * PAGE_SIZE,
				PAGE_SIZE, A9_RR_SKIP_FAULT, false);
	assert(c->nr_ranges == A9_RR_SKIP_RANGES && c->omitted_bytes == 10 * PAGE_SIZE);
	assert(c->scanned + c->skipped == (A9_RR_SKIP_RANGES + 11) * PAGE_SIZE);
}

int main(void)
{
	ram = malloc(FAKE_BYTES);
	assert(ram);
	test_reported_fault();
	test_cross_page_and_revocation();
	test_reserved_opt_in();
	test_limits();
	free(ram);
	puts("PASS: production E1 loop continues past 0x8035f0000; page quarantine, coverage, reserved/no-map skips, rate and stop limits");
	puts("PASS: production reserved opt-in/policy/classifier; crash/CMA/other/mixed hits; no-map, MMIO, invalid/self and AOP-window rejection");
	return 0;
}
