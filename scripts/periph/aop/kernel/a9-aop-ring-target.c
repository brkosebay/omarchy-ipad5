// SPDX-License-Identifier: GPL-2.0-only
/* Frozen, allocation-free postcore capture of explicitly supplied AFK PAs. */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/memblock.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/sched/task_stack.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "a9-aop-ring.h"
#include "a9-aop-ring-target.h"

/* BSS avoids allocating snapshot pages over an inherited heap buffer. */
static struct a9_rt_capture rt_capture;

bool a9_aop_ring_pa_own_page(u64 pa)
{
	phys_addr_t start = __pa_symbol(&rt_capture);

	return pa < start + sizeof(rt_capture) && pa + PAGE_SIZE > start;
}

static bool rt_stack_page(u64 pa)
{
	const u8 *stack = task_stack_page(current);
	struct page *page;
	unsigned int offset;

	for (offset = 0; offset < THREAD_SIZE; offset += PAGE_SIZE) {
		page = is_vmalloc_addr(stack + offset) ? vmalloc_to_page(stack + offset) :
			virt_to_page(stack + offset);
		if (page && page_to_phys(page) == pa)
			return true;
	}
	return false;
}

static bool rt_bad_page(u64 pa)
{
	unsigned int i;

	for (i = 0; i < rt_capture.bad_count; i++)
		if (rt_capture.bad_pa[i] == pa)
			return true;
	return false;
}

/* All counters describe metadata, never a new mapping or a source write. */
static enum a9_rt_reason rt_page_info(struct a9_rt_info *s, u64 pa)
{
	unsigned long pfn = PHYS_PFN(pa);
	bool reserved, system_ram;

	s->pages++;
	if (!memblock_is_region_memory(pa, PAGE_SIZE))
		return A9_RT_NOT_RAM;
	s->ram_pages++;
	system_ram = region_intersects(pa, PAGE_SIZE, IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
				       IORES_DESC_NONE) == REGION_INTERSECTS;
	if (system_ram)
		s->system_ram_pages++;
	reserved = memblock_is_region_reserved(pa, PAGE_SIZE);
	if (!pfn_is_map_memory(pfn)) {
		s->nomap_pages++;
		if (reserved)
			s->reserved_pages++;
		return A9_RT_NOMAP;
	}
	if (!pfn_valid(pfn)) {
		if (reserved)
			s->reserved_pages++;
		return A9_RT_INVALID;
	}
	if (reserved || PageReserved(pfn_to_page(pfn)))
		s->reserved_pages++;
	if (!virt_addr_valid(phys_to_virt(pa)))
		return A9_RT_INVALID;
	s->linear_pages++;
	if (!system_ram)
		return A9_RT_NOT_SYSTEM_RAM;
	if (a9_aop_ring_pa_own_page(pa) || rt_stack_page(pa))
		return A9_RT_SELF;
	if (rt_bad_page(pa))
		return A9_RT_FAULT;
	return A9_RT_OK;
}

static int rt_read_chunk(void *data, u8 *dst, u64 pa, size_t size)
{
	struct a9_rt_sample *s = data;
	struct a9_rt_info info = {};
	u64 page_pa = round_down(pa, PAGE_SIZE);
	enum a9_rt_reason reason;

	reason = rt_page_info(&info, page_pa);
	if (reason != A9_RT_OK) {
		s->info.reason = reason;
		s->info.refused_pa = page_pa;
		return -EACCES;
	}
	if (copy_from_kernel_nofault(dst, phys_to_virt(pa), size)) {
		s->info.reason = A9_RT_FAULT;
		s->info.refused_pa = page_pa;
		if (rt_capture.bad_count < A9_RT_BAD_MAX)
			rt_capture.bad_pa[rt_capture.bad_count++] = page_pa;
		return -EFAULT;
	}
	return 0;
}

static void rt_take(struct a9_rt_target *t, unsigned int sample)
{
	struct a9_rt_sample *s = &t->sample[sample];
	u64 page = round_down(t->request.pa, PAGE_SIZE);
	u64 last = round_down(t->request.pa + t->request.size - 1, PAGE_SIZE), failed;
	enum a9_rt_reason reason;

	s->ns = ktime_get_boottime_ns();
	for (;;) {
		reason = rt_page_info(&s->info, page);
		if (reason != A9_RT_OK && s->info.reason == A9_RT_OK) {
			s->info.reason = reason;
			s->info.refused_pa = page;
		}
		if (page == last)
			break;
		page += PAGE_SIZE;
	}
	/* Refuse the entire range before reading any byte if any page is unsafe. */
	if (s->info.reason != A9_RT_OK) {
		s->error = -EACCES;
		return;
	}
	dma_rmb();
	s->error = a9_rr_copy_pages(s, s->bytes, t->request.pa, t->request.size,
				     PAGE_SIZE, &failed, rt_read_chunk);
}

static void rt_capture_ranges(const struct a9_aop_ring_pa *args, unsigned int count)
{
	struct a9_rt_target *t;
	size_t offset, length;
	unsigned int i;

	if (!count || count > A9_AOP_RING_PA_MAX) {
		rt_capture.error = -EINVAL;
		return;
	}
	rt_capture.count = count;
	get_online_mems();
	for (i = 0; i < count; i++) {
		t = &rt_capture.target[i];
		t->request = args[i];
		if (!args[i].pa || !args[i].size || args[i].size > A9_RT_SIZE_MAX ||
		    args[i].size > ~(u64)0 - args[i].pa) {
			t->error = -ERANGE;
			continue;
		}
		rt_take(t, 0);
		if (!t->sample[0].error &&
		    t->sample[0].info.reserved_pages < t->sample[0].info.pages)
			pr_crit("apple-a9-aop: directed buffer PA 0x%llx includes Linux allocator RAM; inherited AOP DMA may corrupt Linux-owned pages\n",
				args[i].pa);
	}
	msleep(1000);
	for (i = 0; i < count; i++) {
		t = &rt_capture.target[i];
		if (t->error)
			continue;
		rt_take(t, 1);
		if (t->sample[0].error || t->sample[1].error)
			continue;
		for (offset = 0; offset < t->request.size; offset++)
			if (t->sample[0].bytes[offset] != t->sample[1].bytes[offset])
				t->changed_bytes++;
		for (offset = 0; offset < t->request.size; offset += A9_RR_ALIGN) {
			length = min_t(size_t, A9_RR_ALIGN, t->request.size - offset);
			if (memcmp(t->sample[0].bytes + offset,
				   t->sample[1].bytes + offset, length))
				t->changed_lines++;
		}
	}
	put_online_mems();
}

void a9_aop_ring_pa_capture(void)
{
	struct a9_aop_ring_pa args[A9_AOP_RING_PA_MAX];
	int count;

	if (rt_capture.ran)
		return;
	rt_capture.ran = true;
	count = apple_a9_aop_ring_pa_args(args, ARRAY_SIZE(args));
	if (count < 0) {
		rt_capture.error = count;
		return;
	}
	rt_capture_ranges(args, count);
	pr_info("apple-a9-aop: directed PA capture frozen: ranges=%u errno=%d\n",
		rt_capture.count, rt_capture.error);
}

static const char *rt_yes(u32 count, u32 total)
{
	return !count ? "no" : count == total ? "yes" : "mixed";
}

static bool rt_signature(const u8 *bytes, size_t size, size_t offset,
			 struct a9_rr_header *header)
{
	if (offset % A9_RR_ALIGN || offset > size || size - offset < 0x84)
		return false;
	return a9_rr_header(bytes + offset, header);
}

static void rt_dump(struct seq_file *s, const struct a9_rt_target *t, unsigned int sample)
{
	const struct a9_rt_sample *v = &t->sample[sample];
	const struct a9_rt_info *info = &v->info;
	static const char * const reasons[] = {
		"none", "not-ram", "no-map", "invalid-linear-map", "not-system-ram",
		"reader-or-stack", "read-fault-quarantined",
	};
	struct a9_rr_header h;
	size_t offset, length;
	unsigned int matches = 0;

	seq_printf(s, "sample=%u ns=%llu errno=%d reason=%s refused_pa=0x%016llx pages=%u ram_pages=%u system_ram=%s reserved=%s no_map=%s linear_pages=%u unreserved_ram_pages=%u\n",
		   sample, v->ns, v->error, reasons[info->reason], info->refused_pa, info->pages,
		   info->ram_pages,
		   rt_yes(info->system_ram_pages, info->pages),
		   rt_yes(info->reserved_pages, info->pages),
		   rt_yes(info->nomap_pages, info->pages), info->linear_pages,
		   info->ram_pages - info->reserved_pages);
	if (v->error) {
		seq_puts(s, "dump=unavailable partial_data_not_exposed=1\n");
		return;
	}
	seq_printf(s, "signature_at_zero=%s\n", t->request.size < 0x84 ? "unavailable" :
		   rt_signature(v->bytes, t->request.size, 0, &h) ? "yes" : "no");
	for (offset = 0; offset + 0x84 <= t->request.size; offset += A9_RR_ALIGN) {
		if (!rt_signature(v->bytes, t->request.size, offset, &h))
			continue;
		matches++;
		seq_printf(s, "signature offset=0x%zx pa=0x%016llx bufsize=0x%x marker=0x%x rptr=0x%x wptr=0x%x full_ring=%u\n",
			   offset, t->request.pa + offset, h.bufsize, h.marker, h.rptr, h.wptr,
			   t->request.size - offset >= A9_RR_SIZE);
	}
	seq_printf(s, "signature_matches=%u prefix_bytes=%llu\n", matches,
		   min_t(u64, 0x100, t->request.size));
	seq_hex_dump(s, "prefix ", DUMP_PREFIX_OFFSET, 16, 1, v->bytes,
		     min_t(size_t, 0x100, t->request.size), false);
	for (offset = 0; offset < t->request.size; offset += A9_RR_ALIGN) {
		length = min_t(size_t, A9_RR_ALIGN, t->request.size - offset);
		if (!memchr_inv(v->bytes + offset, 0, length))
			continue;
		seq_printf(s, "nonzero64 offset=0x%zx pa=0x%016llx bytes=%zu data=%*phN\n",
			   offset, t->request.pa + offset, length, (int)length, v->bytes + offset);
	}
}

static int rt_show(struct seq_file *s, void *unused)
{
	const struct a9_rt_target *t;
	bool comparable;
	unsigned int i;

	seq_puts(s, "format=1 phase=postcore-initcall frozen=1 hardware_writes=none mailbox=none cache_coherency=unverified\n");
	seq_puts(s, "ownership=lockless-current-state historical_allocation=unknown\n");
	seq_printf(s, "captured=%u config_errno=%d ranges=%u quarantine_pages=%u\n",
		   rt_capture.ran, rt_capture.ran ? rt_capture.error : -ENODATA,
		   rt_capture.count, rt_capture.bad_count);
	for (i = 0; i < rt_capture.count; i++) {
		t = &rt_capture.target[i];
		seq_printf(s, "range=%u pa=0x%016llx size=0x%llx errno=%d\n",
			   i, t->request.pa, t->request.size, t->error);
		if (t->error)
			continue;
		rt_dump(s, t, 0);
		rt_dump(s, t, 1);
		comparable = !t->sample[0].error && !t->sample[1].error;
		seq_printf(s, "changed=%s changed_bytes=%u changed_64b_lines=%u delta_ms=%llu writer=unknown\n",
			   comparable ? (t->changed_bytes ? "yes" : "no") : "unavailable",
			   t->changed_bytes, t->changed_lines,
			   div_u64(t->sample[1].ns - t->sample[0].ns, NSEC_PER_MSEC));
	}
	seq_puts(s, "snapshot_end=1\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rt);

void a9_aop_ring_pa_debugfs(struct dentry *dir)
{
	debugfs_create_file("ring_pa", 0400, dir, NULL, &rt_fops);
}
