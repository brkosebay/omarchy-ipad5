// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zero-traffic reader of an inherited J71t AOP RX ring. No hardware writes,
 * mailbox/FIFO access, cache maintenance, late reservation or DMA API calls.
 * Discovery only uses fault-contained reads of valid linear-mapped RAM.
 * IIO only uses an early DT no-map RAM reservation with a RO WB mapping.
 */
#include <linux/bitmap.h>
#include <linux/cma.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/memblock.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/sched/task_stack.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "a9-aop-ring.h"
#include "a9-aop-ring-reader.h"
#include "a9-aop-ring-target.h"
#include "a9-aop.h"

#define RR_HITS		16
#define RR_MAX_BYTES	(2ULL << 30)
#define RR_MAX_SPAN	(4ULL << 30)
#define RR_SCAN_NS	(45ULL * NSEC_PER_SEC)
#define RR_WATCH_MS	2000
#define RR_POLL_MS	20
#define RR_STALE_NS	(500ULL * NSEC_PER_MSEC)

struct rr_hit {
	phys_addr_t pa;
	enum a9_rr_range_class range_class;
	struct a9_rr_header first;
	struct a9_rr_header last;
	struct a9_rr_report report;
	u8 prefix[0x100];
	u64 watch_ns;
	u64 last_ns;
	u32 watch_wptr;
	u32 changes;
	u32 reserved_pages;
	u32 referenced_pages;
	u32 buddy_pages;
	u32 allocator_pages;
	int parse_errno;
	bool warned;
	bool unreadable;
};

struct a9_ring_reader {
	struct device *dev;
	struct mutex lock; /* Serializes the one-shot scan, poll and sysfs. */
	struct rr_hit hits[RR_HITS];
	u8 *page;
	u8 *snapshot;
	u64 scan_start_ns;
	u64 scan_end_ns;
	struct a9_rr_coverage coverage;
	unsigned long *read_pages;
	unsigned long *skip_pages;
	unsigned long base_pfn;
	unsigned long nr_pages;
	u64 ram_bytes;
	u64 walked;
	unsigned int nr_hits;
	int scan_errno;
	bool scan_done;
	bool scan_reserved;
	const char *scan_stop;
	phys_addr_t fault_pa;
	phys_addr_t scan_fault_pa;
	const char *scan_phase;
	/* E2 state, kept separate from unreserved discovery hits. */
	const u8 *ring;
	void __iomem *map;
	phys_addr_t ring_pa;
	struct a9_rr_header header;
	struct a9_rr_report latest;
	u32 cursor;
	u32 last_wptr;
	u64 received_ns;
	u64 wptr_ns;
	u64 samples;
	int iio_errno;
	bool valid;
	bool failed;
	struct delayed_work work;
#if IS_REACHABLE(CONFIG_IIO)
	struct iio_mount_matrix matrix;
#endif
};

static bool rr_linear_page(phys_addr_t pa)
{
	unsigned long pfn = PHYS_PFN(pa);

	return pfn_valid(pfn) && pfn_is_map_memory(pfn) &&
	       virt_addr_valid(phys_to_virt(pa));
}

static bool rr_contains_page(const void *ptr, size_t size, phys_addr_t pa)
{
	phys_addr_t start = virt_to_phys(ptr);

	return pa >= round_down(start, PAGE_SIZE) && pa < PAGE_ALIGN(start + size);
}

static int rr_scan_page_reason(struct a9_ring_reader *rr, phys_addr_t pa)
{
	const u8 *stack = task_stack_page(current);
	struct page *page;
	unsigned long index = PHYS_PFN(pa) - rr->base_pfn;
	unsigned int offset;

	if (index >= rr->nr_pages)
		return A9_RR_SKIP_INVALID;
	if (test_bit(index, rr->skip_pages))
		return A9_RR_SKIP_INVALID; /* Already accounted; never retry. */
	if (!pfn_is_map_memory(PHYS_PFN(pa)))
		return A9_RR_SKIP_NOMAP;
	if (!rr->scan_reserved && memblock_is_region_reserved(pa, PAGE_SIZE))
		return A9_RR_SKIP_RESERVED;
	if (!rr_linear_page(pa))
		return A9_RR_SKIP_INVALID;
	if (!rr->scan_reserved && PageReserved(pfn_to_page(PHYS_PFN(pa))))
		return A9_RR_SKIP_RESERVED;
	if (!memblock_is_region_memory(pa, PAGE_SIZE) ||
	    region_intersects(pa, PAGE_SIZE, IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
			      IORES_DESC_NONE) != REGION_INTERSECTS)
		return A9_RR_SKIP_RESOURCE;
	/* Never rediscover our own copied headers as apparent live DMA rings. */
	if (rr_contains_page(rr, sizeof(*rr), pa) ||
	    a9_aop_ring_pa_own_page(pa) ||
	    rr_contains_page(rr->page, PAGE_SIZE, pa) ||
	    rr_contains_page(rr->snapshot, A9_RR_SIZE, pa) ||
	    rr_contains_page(rr->read_pages, bitmap_size(rr->nr_pages), pa) ||
	    rr_contains_page(rr->skip_pages, bitmap_size(rr->nr_pages), pa))
		return A9_RR_SKIP_SELF;
	for (offset = 0; offset < THREAD_SIZE; offset += PAGE_SIZE) {
		page = is_vmalloc_addr(stack + offset) ? vmalloc_to_page(stack + offset) :
			virt_to_page(stack + offset);
		if (page && page_to_phys(page) == pa)
			return A9_RR_SKIP_SELF;
	}
	return -1;
}

#if IS_ENABLED(CONFIG_CMA)
static int rr_cma_page(struct cma *cma, void *data)
{
	phys_addr_t pa = *(phys_addr_t *)data;

	/* This also handles discontiguous CMA areas, without cma_get_base(). */
	return cma_intersects(cma, pa, pa + PAGE_SIZE - 1);
}
#endif

/* Informational membership, not a claim that the bytes were never written. */
static enum a9_rr_range_class rr_page_class(phys_addr_t pa)
{
	if (region_intersects(pa, PAGE_SIZE, IORESOURCE_MEM,
			      IORES_DESC_CRASH_KERNEL) == REGION_INTERSECTS)
		return A9_RR_CLASS_CRASH;
#if IS_ENABLED(CONFIG_CMA)
	if (cma_for_each_area(rr_cma_page, &pa))
		return A9_RR_CLASS_CMA;
#endif
	if (memblock_is_region_reserved(pa, PAGE_SIZE) ||
	    PageReserved(pfn_to_page(PHYS_PFN(pa))))
		return A9_RR_CLASS_RESERVED;
	return A9_RR_CLASS_RAM;
}

static enum a9_rr_range_class rr_hit_class(phys_addr_t pa)
{
	phys_addr_t page = round_down(pa, PAGE_SIZE);
	enum a9_rr_range_class class = rr_page_class(page);

	for (page += PAGE_SIZE; page < pa + A9_RR_SIZE; page += PAGE_SIZE)
		if (rr_page_class(page) != class)
			return A9_RR_CLASS_MIXED;
	return class;
}

static void rr_skip_page(struct a9_ring_reader *rr, phys_addr_t pa,
			 enum a9_rr_skip_reason reason)
{
	unsigned long index = PHYS_PFN(pa) - rr->base_pfn;
	bool was_read;

	if (index >= rr->nr_pages || test_and_set_bit(index, rr->skip_pages))
		return;
	was_read = test_and_clear_bit(index, rr->read_pages);
	a9_rr_skip_page(&rr->coverage, pa, PAGE_SIZE, reason, was_read);
}

static int rr_scan_chunk(void *data, u8 *dst, u64 pa, size_t size)
{
	struct a9_ring_reader *rr = data;
	phys_addr_t page_pa = round_down(pa, PAGE_SIZE);
	int reason = rr_scan_page_reason(rr, page_pa);

	if (reason >= 0) {
		rr_skip_page(rr, page_pa, reason);
		return -EACCES;
	}
	if (copy_from_kernel_nofault(dst, phys_to_virt(pa), size)) {
		rr->scan_fault_pa = page_pa;
		rr_skip_page(rr, page_pa, A9_RR_SKIP_FAULT);
		return -EFAULT;
	}
	return 0;
}

static int rr_scan_copy(struct a9_ring_reader *rr, u8 *dst, phys_addr_t pa, size_t size)
{
	u64 failed_page;

	return a9_rr_copy_pages(rr, dst, pa, size, PAGE_SIZE, &failed_page, rr_scan_chunk);
}

/* E2 faults still latch. E1 uses page-sized copies and quarantines bad PFNs. */
static int rr_copy(struct a9_ring_reader *rr, void *dst, const void *src,
		   phys_addr_t pa, size_t size)
{
	if (copy_from_kernel_nofault(dst, src, size)) {
		rr->fault_pa = pa;
		return -EFAULT;
	}
	return 0;
}

/* Two equal published pointers bracket the private copy. No cache aliases. */
static int rr_snapshot(struct a9_ring_reader *rr, const u8 *src,
		       phys_addr_t pa, struct a9_rr_header *h, bool scan)
{
	u8 before[0x84], after[0x84];
	struct a9_rr_header a, b;
	int ret;

	ret = scan ? rr_scan_copy(rr, before, pa, sizeof(before)) :
		rr_copy(rr, before, src, pa, sizeof(before));
	if (ret)
		return ret;
	if (!a9_rr_header(before, &a))
		return -EPROTO;
	/* Firmware must publish payload before wptr (coherency still needs E1). */
	dma_rmb();
	ret = scan ? rr_scan_copy(rr, rr->snapshot, pa, A9_RR_SIZE) :
		rr_copy(rr, rr->snapshot, src, pa, A9_RR_SIZE);
	if (ret)
		return ret;
	dma_rmb();
	ret = scan ? rr_scan_copy(rr, after, pa, sizeof(after)) :
		rr_copy(rr, after, src, pa, sizeof(after));
	if (ret)
		return ret;
	if (!a9_rr_header(after, &b))
		return -EPROTO;
	if (a.rptr != b.rptr || a.wptr != b.wptr ||
	    get_unaligned_le32(rr->snapshot + 0x40) != a.rptr ||
	    get_unaligned_le32(rr->snapshot + 0x80) != a.wptr ||
	    get_unaligned_le32(rr->snapshot) != A9_RR_BUFSIZE ||
	    get_unaligned_le32(rr->snapshot + 4) != A9_RR_MARKER)
		return -EAGAIN;
	*h = b;
	return 0;
}

/*
 * Advisory, lockless state only; do not take references/allocate the pages.
 * PageBuddy describes free-block heads, not every free page (PCP included).
 * Nonreserved pages have been given to the allocator even if currently free.
 * A zero refcount cannot prove that a page was never previously allocated.
 */
static void rr_page_state(struct a9_ring_reader *rr, struct rr_hit *hit)
{
	phys_addr_t pa, end = hit->pa + A9_RR_SIZE;
	struct page *page;

	hit->reserved_pages = 0;
	hit->referenced_pages = 0;
	hit->buddy_pages = 0;
	hit->allocator_pages = 0;
	for (pa = round_down(hit->pa, PAGE_SIZE); pa < end; pa += PAGE_SIZE) {
		page = pfn_to_page(PHYS_PFN(pa));
		if (PageReserved(page))
			hit->reserved_pages++;
		else
			hit->allocator_pages++;
		if (page_ref_count(page) > 0)
			hit->referenced_pages++;
		if (PageBuddy(page))
			hit->buddy_pages++;
	}
	if (hit->allocator_pages && !hit->warned) {
		if (rr->dev)
			dev_crit(rr->dev,
			 "AOP ring candidate %pa in Linux allocator RAM: referenced=%u; possible inherited DMA corruption! Reserve before next boot; no late repair\n",
			 &hit->pa, hit->referenced_pages);
		else
			pr_crit("apple-a9-aop: ring candidate %pa in Linux allocator RAM: referenced=%u; possible inherited DMA corruption!\n",
				&hit->pa, hit->referenced_pages);
		hit->warned = true;
	}
}

static int rr_scan_range(struct resource *res, void *data)
{
	struct a9_ring_reader *rr = data;
	struct a9_rr_header header;
	struct rr_hit *hit;
	phys_addr_t pa;
	u32 offset;
	int ret;

	for (pa = ALIGN(res->start, PAGE_SIZE);
	     pa <= res->end && res->end - pa >= PAGE_SIZE - 1; pa += PAGE_SIZE) {
		if (fatal_signal_pending(current)) {
			rr->scan_stop = "signal";
			return -EINTR;
		}
		if (rr->walked >= RR_MAX_BYTES ||
		    ktime_get_boottime_ns() - rr->scan_start_ns >= RR_SCAN_NS) {
			rr->scan_stop = "budget";
			return 1;
		}
		rr->walked += PAGE_SIZE;
		ret = rr_scan_copy(rr, rr->page, pa, PAGE_SIZE);
		if (ret == -EFAULT || ret == -EACCES)
			goto next_page;
		if (ret)
			return ret;
		set_bit(PHYS_PFN(pa) - rr->base_pfn, rr->read_pages);
		rr->coverage.scanned += PAGE_SIZE;
		for (offset = 0; offset < PAGE_SIZE; offset += A9_RR_ALIGN) {
			/* The cheap signature is wholly inside this page. */
			if (get_unaligned_le32(rr->page + offset) != A9_RR_BUFSIZE ||
			    get_unaligned_le32(rr->page + offset + 4) != A9_RR_MARKER)
				continue;
			if (round_down(res->end + 1, PAGE_SIZE) - (pa + offset) < A9_RR_SIZE)
				continue;
			ret = rr_scan_copy(rr, rr->snapshot, pa + offset, A9_RR_SIZE);
			if (ret)
				continue;
			if (!a9_rr_header(rr->snapshot, &header))
				continue;
			hit = &rr->hits[rr->nr_hits++];
			hit->pa = pa + offset;
			hit->range_class = rr_hit_class(hit->pa);
			hit->first = header;
			hit->last = header;
			memcpy(hit->prefix, rr->snapshot, sizeof(hit->prefix));
			rr_page_state(rr, hit);
			if (rr->nr_hits == RR_HITS) {
				rr->scan_stop = "hit-limit";
				return 1;
			}
		}
next_page:
		/* Pace skipped/faulting pages too; successful reads cannot exceed this. */
		if (!(rr->walked % (256 * 1024)))
			usleep_range(4000, 5000);
		cond_resched();
	}
	return 0;
}

static int rr_watch(struct a9_ring_reader *rr)
{
	struct rr_hit *hit;
	struct a9_rr_header h;
	u64 start = ktime_get_boottime_ns();
	u32 next;
	unsigned int i;
	int ret;

	/* Observe all hits together for two seconds, not two seconds per hit. */
	do {
		if (msleep_interruptible(100))
			return -EINTR;
		for (i = 0; i < rr->nr_hits; i++) {
			hit = &rr->hits[i];
			if (hit->unreadable)
				continue;
			ret = rr_snapshot(rr, phys_to_virt(hit->pa), hit->pa, &h, true);
			if (ret) {
				hit->parse_errno = ret;
				if (ret == -EFAULT || ret == -EACCES)
					hit->unreadable = true;
				continue;
			}
			if (!hit->watch_ns) {
				/* Discovery may precede this watch by a full RAM scan. */
				hit->watch_ns = ktime_get_boottime_ns();
				hit->watch_wptr = h.wptr;
			} else if (h.wptr != hit->last.wptr) {
				hit->changes++;
			}
			hit->last = h;
			hit->last_ns = ktime_get_boottime_ns();
			hit->parse_errno = a9_rr_decode(rr->snapshot, h.rptr, h.wptr,
						      &next, &hit->report);
		}
	} while (ktime_get_boottime_ns() - start < RR_WATCH_MS * NSEC_PER_MSEC);
	for (i = 0; i < rr->nr_hits; i++) {
		hit = &rr->hits[i];
		rr_page_state(rr, hit);
		if (hit->changes && hit->allocator_pages) {
			if (rr->dev)
				dev_crit(rr->dev,
				 "LIVE AOP ring candidate %pa: wptr advanced %u times in Linux-owned RAM; latent DMA corruption risk\n",
				 &hit->pa, hit->changes);
			else
				pr_crit("apple-a9-aop: LIVE ring candidate %pa: wptr advanced %u times in Linux-owned RAM; latent DMA corruption risk\n",
					&hit->pa, hit->changes);
		}
	}
	return 0;
}

static void rr_run_scan(struct a9_ring_reader *rr)
{
	struct memblock_region *region;
	struct resource res;
	int ret = 0;

	rr->scan_done = true;
	rr->scan_stop = "end-of-ram";
	rr->scan_start_ns = ktime_get_boottime_ns();
	get_online_mems();
	/* Inventory no-map RAM as skipped too; only System RAM can pass read policy. */
	for_each_mem_region(region) {
		if (!region->size)
			continue;
		res.start = region->base;
		res.end = region->base + region->size - 1;
		ret = rr_scan_range(&res, rr);
		if (ret)
			break;
	}
	if (ret < 0) {
		rr->scan_errno = ret;
		rr->scan_stop = "aborted";
	} else if (rr->nr_hits) {
		rr->scan_errno = rr_watch(rr);
		if (rr->scan_errno)
			rr->scan_stop = "aborted";
	}
	put_online_mems();
	rr->scan_end_ns = ktime_get_boottime_ns();
}

static int rr_scan_show(struct seq_file *s, void *unused)
{
	struct a9_ring_reader *rr = s->private;
	struct rr_hit *hit;
	unsigned int i;
	struct a9_rr_coverage *c = &rr->coverage;
	static const char * const reasons[] = {
		"no-map", "reserved", "not-system-ram", "invalid-linear-map",
		"reader-or-stack", "read-fault",
	};
	static const char * const classes[] = {
		"system-ram", "crash-kernel", "cma", "reserved-dram", "mixed",
	};
	u64 unvisited;

	if (mutex_lock_interruptible(&rr->lock))
		return -EINTR;
	if (!rr->scan_done)
		rr_run_scan(rr);
	unvisited = rr->ram_bytes - c->scanned - c->skipped;
	seq_puts(s, "format=2 hardware_writes=none mailbox=none rptr_writes=none frozen=1\n");
	seq_puts(s, "ownership=lockless-current-state historical_allocation=unknown cache_coherency=unverified\n");
	seq_puts(s, "excluded=reader-allocations-and-current-kernel-stack\n");
	seq_printf(s, "ring_scan_reserved=%u class_source=kernel-resource-and-cma-metadata\n",
		   rr->scan_reserved);
	seq_printf(s, "scan_stop=%s errno=%d bytes=%llu skipped=%llu hits=%u elapsed_ms=%llu fault_pa=%pa\n",
		   rr->scan_stop, rr->scan_errno, c->scanned, c->skipped, rr->nr_hits,
		   div_u64(rr->scan_end_ns - rr->scan_start_ns, NSEC_PER_MSEC), &rr->scan_fault_pa);
	seq_printf(s, "phase=%s start_boottime_ns=%llu ram_bytes=%llu walked_bytes=%llu unvisited_bytes=%llu walk_complete=%u fault_pages=%llu\n",
		   rr->scan_phase, rr->scan_start_ns, rr->ram_bytes, rr->walked,
		   unvisited, rr->walked == rr->ram_bytes,
		   div_u64(c->by_reason[A9_RR_SKIP_FAULT], PAGE_SIZE));
	seq_printf(s, "skipped_ranges=%u ranges_end=exclusive omitted_skipped_bytes=%llu\n",
		   c->nr_ranges, c->omitted_bytes);
	for (i = 0; i < A9_RR_SKIP_REASONS; i++)
		seq_printf(s, "skipped_reason=%s bytes=%llu\n", reasons[i], c->by_reason[i]);
	for (i = 0; i < c->nr_ranges; i++)
		seq_printf(s, "skip start=0x%016llx end=0x%016llx reason=%s bytes=%llu\n",
			   c->ranges[i].start, c->ranges[i].end, reasons[c->ranges[i].reason],
			   c->ranges[i].end - c->ranges[i].start);
	for (i = 0; i < rr->nr_hits; i++) {
		hit = &rr->hits[i];
		seq_printf(s, "hit=%u pa=%pa range_class=%s bufsize=0x%x marker=0x%x rptr=0x%x wptr_discovery=0x%x wptr_watch_first=0x%x wptr_last=0x%x changes=%u advanced=%s watch_ms=%llu\n",
			   i, &hit->pa, classes[hit->range_class],
			   hit->first.bufsize, hit->first.marker, hit->last.rptr,
			   hit->first.wptr, hit->watch_wptr,
			   hit->last.wptr, hit->changes,
			   hit->changes ? "yes" : "no",
			   hit->last_ns ? div_u64(hit->last_ns - hit->watch_ns,
						 NSEC_PER_MSEC) : 0);
		seq_printf(s, "allocator_pages=%u referenced_pages=%u reserved_pages=%u buddy_heads=%u parse_errno=%d magic=0x%x length=%u channel=%u category=0x%x subtype=0x%x reports=%u seq=%u timestamp=%llu axes=%d,%d,%d\n",
			   hit->allocator_pages, hit->referenced_pages, hit->reserved_pages,
			   hit->buddy_pages, hit->parse_errno, hit->report.magic,
			   hit->report.length, hit->report.channel, hit->report.category,
			   hit->report.subtype, hit->report.reports, hit->report.motion.sequence,
			   hit->report.timestamp, hit->report.motion.axes[0],
			   hit->report.motion.axes[1], hit->report.motion.axes[2]);
		seq_hex_dump(s, "header ", DUMP_PREFIX_OFFSET, 16, 1,
			     hit->prefix, sizeof(hit->prefix), false);
	}
	seq_puts(s, "snapshot_end=1\n");
	mutex_unlock(&rr->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rr_scan);

static int rr_iio_show(struct seq_file *s, void *unused)
{
	struct a9_ring_reader *rr = s->private;
	u64 now;

	mutex_lock(&rr->lock);
	now = ktime_get_boottime_ns();
	seq_printf(s, "pa=%pa errno=%d failed=%u valid=%u samples=%llu cursor=0x%x wptr=0x%x mapping=ro-wb hardware_writes=none rptr_writes=none\n",
		   &rr->ring_pa, rr->iio_errno, rr->failed, rr->valid,
		   rr->samples, rr->cursor, rr->last_wptr);
	seq_printf(s, "bufsize=0x%x marker=0x%x rptr=0x%x fresh=%u received_ns=%llu wptr_ns=%llu fault_pa=%pa\n",
		   rr->header.bufsize, rr->header.marker, rr->header.rptr,
		   rr->valid && !rr->failed && now - rr->received_ns <= RR_STALE_NS &&
		   now - rr->wptr_ns <= RR_STALE_NS, rr->received_ns, rr->wptr_ns,
		   &rr->fault_pa);
	seq_puts(s, "snapshot_end=1\n");
	mutex_unlock(&rr->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rr_iio);

#if IS_REACHABLE(CONFIG_IIO)
/* The shared reservation must have no other memory-region consumer. */
static bool rr_exclusive(struct device_node *owner, struct device_node *target)
{
	struct device_node *node, *ref;
	int i, count;

	for_each_of_allnodes(node) {
		if (node == owner)
			continue;
		count = of_count_phandle_with_args(node, "memory-region", NULL);
		for (i = 0; i < count; i++) {
			ref = of_parse_phandle(node, "memory-region", i);
			if (ref == target) {
				of_node_put(ref);
				of_node_put(node);
				return false;
			}
			of_node_put(ref);
		}
	}
	return true;
}

static bool rr_overlaps(struct device_node *parent, struct device_node *target,
			struct reserved_mem *rmem)
{
	struct device_node *node;
	struct reserved_mem *other;
	struct resource res;
	unsigned int i;

	for_each_available_child_of_node(parent, node) {
		if (node == target)
			continue;
		/* Also consider allocated dynamic reserved-memory nodes. */
		other = of_reserved_mem_lookup(node);
		if (other && other->size && other->base < rmem->base + rmem->size &&
		    rmem->base < other->base + other->size)
			goto overlap;
		for (i = 0; !of_address_to_resource(node, i, &res); i++)
			if (res.start < rmem->base + rmem->size && rmem->base <= res.end)
				goto overlap;
	}
	return false;
overlap:
	of_node_put(node);
	return true;
}

static void rr_unmap(void *data)
{
	struct a9_ring_reader *rr = data;

	iounmap(rr->map);
}

/* No late memblock_reserve: DT must exclude these pages before allocation. */
static int rr_map_reserved(struct a9_ring_reader *rr)
{
	struct device_node *np, *parent;
	struct reserved_mem *rmem;
	struct resource res;
	phys_addr_t pa;
	u32 offset = 0;
	int ret = -EINVAL;

	if (of_count_phandle_with_args(rr->dev->of_node, "memory-region", NULL) != 1)
		return -EINVAL;
	np = of_parse_phandle(rr->dev->of_node, "memory-region", 0);
	if (!np)
		return -EINVAL;
	parent = of_get_parent(np);
	if (!parent || strcmp(parent->full_name, "/reserved-memory") ||
	    !of_device_is_available(np) || !of_property_read_bool(np, "no-map") ||
	    of_property_read_bool(np, "reusable") || of_find_property(np, "compatible", NULL) ||
	    !rr_exclusive(rr->dev->of_node, np))
		goto out;
	rmem = of_reserved_mem_lookup(np);
	if (!rmem || rmem->ops || of_address_to_resource(np, 0, &res) ||
	    rmem->base != res.start || rmem->size != resource_size(&res))
		goto out;
	/* Accept one static region only; reject extra tuples or overlapping owners. */
	if (!of_address_to_resource(np, 1, &res))
		goto out;
	if (of_find_property(rr->dev->of_node, "apple,ring-offset", NULL) &&
	    of_property_read_u32(rr->dev->of_node, "apple,ring-offset", &offset))
		goto out;
	if (!PAGE_ALIGNED(rmem->base) || offset >= PAGE_SIZE || offset % A9_RR_ALIGN ||
	    rmem->size != PAGE_ALIGN(offset + A9_RR_SIZE) ||
	    rmem->base + rmem->size < rmem->base ||
	    !memblock_is_region_memory(rmem->base, rmem->size))
		goto out;
	if (rr_overlaps(parent, np, rmem))
		goto out;
	for (pa = rmem->base; pa < rmem->base + rmem->size; pa += PAGE_SIZE)
		if (pfn_is_map_memory(PHYS_PFN(pa)))
			goto out;
	/* arm64 rejects ioremap of mapped RAM; no-map permits one RO normal alias. */
	rr->map = __ioremap_prot(rmem->base, rmem->size, PAGE_KERNEL_RO);
	if (!rr->map) {
		ret = -ENOMEM;
		goto out;
	}
	rr->ring = (const u8 __force *)rr->map + offset;
	rr->ring_pa = rmem->base + offset;
	ret = devm_add_action_or_reset(rr->dev, rr_unmap, rr);
out:
	of_node_put(parent);
	of_node_put(np);
	return ret;
}

/* Called under lock, or before debugfs/IIO publication during probe. */
static int rr_poll(struct a9_ring_reader *rr)
{
	struct a9_rr_report report;
	struct a9_rr_header h;
	u64 now = ktime_get_boottime_ns();
	u32 next;
	int ret;

	ret = rr_snapshot(rr, rr->ring, rr->ring_pa, &h, false);
	if (ret == -EAGAIN)
		return 0;
	if (ret)
		goto fail;
	if (h.wptr == rr->last_wptr)
		return 0;
	rr->wptr_ns = now;
	rr->header = h;
	ret = a9_rr_decode(rr->snapshot, rr->cursor, h.wptr, &next, &report);
	if (ret)
		goto fail;
	rr->last_wptr = h.wptr;
	rr->cursor = next; /* Private cursor; shared rptr is NEVER acknowledged. */
	if (report.reports && (!rr->samples ||
	    report.motion.sequence != rr->latest.motion.sequence ||
	    report.timestamp != rr->latest.timestamp)) {
		rr->latest = report;
		rr->received_ns = now;
		rr->samples += report.reports;
		rr->valid = true;
	}
	return 0;
fail:
	rr->failed = true;
	rr->valid = false;
	rr->iio_errno = ret;
	dev_err(rr->dev, "reserved ring reader stopped: errno=%d; no retry or writes\n", ret);
	return ret;
}

static void rr_work(struct work_struct *work)
{
	struct a9_ring_reader *rr = container_of(to_delayed_work(work),
						struct a9_ring_reader, work);

	mutex_lock(&rr->lock);
	rr_poll(rr);
	if (!rr->failed)
		schedule_delayed_work(&rr->work, msecs_to_jiffies(RR_POLL_MS));
	mutex_unlock(&rr->lock);
}

static void rr_cancel(void *data)
{
	struct a9_ring_reader *rr = data;

	cancel_delayed_work_sync(&rr->work);
}

static const struct iio_mount_matrix *
rr_matrix(const struct iio_dev *indio, const struct iio_chan_spec *chan)
{
	struct a9_ring_reader *rr = *(struct a9_ring_reader **)iio_priv(indio);

	return &rr->matrix;
}

static const struct iio_chan_spec_ext_info rr_ext[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, rr_matrix),
	{}
};

#define RR_CHANNEL(axis, index) { \
	.type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_##axis, \
	.address = index, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), .ext_info = rr_ext, \
}

static const struct iio_chan_spec rr_channels[] = {
	RR_CHANNEL(X, 0), RR_CHANNEL(Y, 1), RR_CHANNEL(Z, 2),
};

static int rr_read_raw(struct iio_dev *indio, const struct iio_chan_spec *chan,
		       int *val, int *val2, long mask)
{
	struct a9_ring_reader *rr = *(struct a9_ring_reader **)iio_priv(indio);
	u64 now;
	int ret = -ENODATA;

	if (mask == IIO_CHAN_INFO_SCALE) {
		/* MPU-6900 AOP report units: 16.16 g, not chip-register counts. */
		*val = 0;
		*val2 = 149638;
		return IIO_VAL_INT_PLUS_NANO;
	}
	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	mutex_lock(&rr->lock);
	now = ktime_get_boottime_ns();
	if (rr->valid && !rr->failed && now - rr->received_ns <= RR_STALE_NS &&
	    now - rr->wptr_ns <= RR_STALE_NS) {
		*val = rr->latest.motion.axes[chan->address];
		ret = IIO_VAL_INT;
	}
	mutex_unlock(&rr->lock);
	return ret;
}

static const struct iio_info rr_info = {
	.read_raw = rr_read_raw,
};

static int rr_start_iio(struct a9_ring_reader *rr)
{
	struct iio_dev *indio;
	u64 start;
	int ret;

	ret = rr_map_reserved(rr);
	if (ret)
		return ret;
	ret = rr_snapshot(rr, rr->ring, rr->ring_pa, &rr->header, false);
	if (ret)
		return ret;
	/* Ignore historical entries: require a NEW publication after mapping. */
	rr->cursor = rr->header.wptr;
	rr->last_wptr = rr->header.wptr;
	start = ktime_get_boottime_ns();
	do {
		if (msleep_interruptible(RR_POLL_MS))
			return -EINTR;
		ret = rr_poll(rr);
		if (ret)
			return ret;
	} while (!rr->valid &&
		 ktime_get_boottime_ns() - start < RR_WATCH_MS * NSEC_PER_MSEC);
	if (!rr->valid)
		return -ENODATA;
	ret = iio_read_mount_matrix(rr->dev, &rr->matrix);
	if (ret)
		return ret;
	indio = devm_iio_device_alloc(rr->dev, sizeof(rr));
	if (!indio)
		return -ENOMEM;
	*(struct a9_ring_reader **)iio_priv(indio) = rr;
	indio->name = "apple-a9-aop-ring-accel";
	indio->info = &rr_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = rr_channels;
	indio->num_channels = ARRAY_SIZE(rr_channels);
	INIT_DELAYED_WORK(&rr->work, rr_work);
	ret = devm_add_action_or_reset(rr->dev, rr_cancel, rr);
	if (ret)
		return ret;
	/* Work only touches rr, never indio; drain it before rr/map are freed. */
	ret = devm_iio_device_register(rr->dev, indio);
	if (ret)
		return ret;
	schedule_delayed_work(&rr->work, msecs_to_jiffies(RR_POLL_MS));
	dev_info(rr->dev, "reserved AOP ring %pa: fresh accel report, RO IIO ready\n",
		 &rr->ring_pa);
	return 0;
}
#else
static int rr_start_iio(struct a9_ring_reader *rr)
{
	return -EOPNOTSUPP;
}
#endif

static void rr_free(void *data)
{
	struct a9_ring_reader *rr = data;

	bitmap_free(rr->read_pages);
	bitmap_free(rr->skip_pages);
	kfree(rr->snapshot);
	kfree(rr->page);
	kfree(rr);
}

static struct a9_ring_reader *rr_alloc(void)
{
	struct a9_ring_reader *rr;
	struct memblock_region *region;
	phys_addr_t start = memblock_start_of_DRAM(), end = memblock_end_of_DRAM();
	u64 lo, hi;
	int ret = -ENOMEM;

	rr = kzalloc_obj(*rr);
	if (!rr)
		return ERR_PTR(-ENOMEM);
	mutex_init(&rr->lock);
	rr->scan_phase = "debugfs";
	rr->scan_reserved = apple_a9_aop_ring_scan_reserved();
	if (end <= start || end - start > RR_MAX_SPAN) {
		ret = -ERANGE;
		goto fail;
	}
	rr->base_pfn = PHYS_PFN(start);
	rr->nr_pages = PFN_UP(end) - rr->base_pfn;
	rr->snapshot = kmalloc(A9_RR_SIZE, GFP_KERNEL);
	rr->page = kmalloc(PAGE_SIZE, GFP_KERNEL);
	rr->read_pages = bitmap_zalloc(rr->nr_pages, GFP_KERNEL);
	rr->skip_pages = bitmap_zalloc(rr->nr_pages, GFP_KERNEL);
	if (!rr->snapshot || !rr->page || !rr->read_pages || !rr->skip_pages)
		goto fail;
	for_each_mem_region(region) {
		lo = ALIGN(region->base, PAGE_SIZE);
		hi = round_down(region->base + region->size, PAGE_SIZE);
		if (hi > lo)
			rr->ram_bytes += hi - lo;
	}
	return rr;
fail:
	rr_free(rr);
	return ERR_PTR(ret);
}

static struct a9_ring_reader *early_rr;
static int early_errno;

static int __init rr_early_init(void)
{
	/* Boot-time research excludes RAM captures even with conflicting tokens. */
	if (apple_a9_aop_attach_stage())
		return 0;
	if (!of_machine_is_compatible("apple,j71t"))
		return 0;
	/* Exact PA evidence must precede both the allocator-backed reader and sweep. */
	if (apple_a9_aop_ring_pa_enabled())
		a9_aop_ring_pa_capture();
	if (!apple_a9_aop_ring_scan_early())
		return 0;
	early_rr = rr_alloc();
	if (IS_ERR(early_rr)) {
		early_errno = PTR_ERR(early_rr);
		early_rr = NULL;
		pr_err("apple-a9-aop: early ring scan allocation failed: %d\n", early_errno);
		return 0;
	}
	early_rr->scan_phase = "postcore-initcall";
	mutex_lock(&early_rr->lock);
	rr_run_scan(early_rr);
	mutex_unlock(&early_rr->lock);
	pr_info("apple-a9-aop: early ring scan frozen: stop=%s bytes=%llu skipped=%llu faults=%llu hits=%u\n",
		early_rr->scan_stop, early_rr->coverage.scanned, early_rr->coverage.skipped,
		div_u64(early_rr->coverage.by_reason[A9_RR_SKIP_FAULT], PAGE_SIZE),
		early_rr->nr_hits);
	return 0;
}
postcore_initcall_sync(rr_early_init);

int a9_aop_ring_reader_init(struct device *dev, struct dentry *dir,
			    bool scan, bool iio)
{
	struct a9_ring_reader *rr;
	int ret;

	if (apple_a9_aop_ring_pa_enabled())
		a9_aop_ring_pa_debugfs(dir);
	if (!scan && !iio)
		return 0;
	if (scan && early_rr) {
		rr = early_rr;
		early_rr = NULL;
	} else {
		rr = rr_alloc();
		if (IS_ERR(rr))
			return PTR_ERR(rr);
		if (scan && apple_a9_aop_ring_scan_early()) {
			/* Never silently substitute a late capture for a failed early one. */
			rr->scan_done = true;
			rr->scan_phase = "early-unavailable";
			rr->scan_stop = "early-init-failed";
			rr->scan_errno = early_errno ? early_errno : -ENODATA;
		}
	}
	rr->dev = dev;
	ret = devm_add_action_or_reset(dev, rr_free, rr);
	if (ret)
		return ret;
	if (iio) {
		rr->iio_errno = rr_start_iio(rr);
		if (rr->iio_errno) {
			rr->failed = true;
			rr->valid = false;
			dev_warn(dev, "ring IIO refused: errno=%d; require early no-map RAM and fresh reports\n",
				 rr->iio_errno);
		}
		debugfs_create_file("ring_reader", 0400, dir, rr, &rr_iio_fops);
	}
	if (scan)
		debugfs_create_file("ring_scan", 0400, dir, rr, &rr_scan_fops);
	return 0;
}
