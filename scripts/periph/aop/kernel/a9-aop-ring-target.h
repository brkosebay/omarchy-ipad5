/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _A9_AOP_RING_TARGET_H
#define _A9_AOP_RING_TARGET_H

#include "a9-aop.h"

#define A9_RT_SIZE_MAX 0x10000
#define A9_RT_BAD_MAX (2 * A9_AOP_RING_PA_MAX)

enum a9_rt_reason {
	A9_RT_OK,
	A9_RT_NOT_RAM,
	A9_RT_NOMAP,
	A9_RT_INVALID,
	A9_RT_NOT_SYSTEM_RAM,
	A9_RT_SELF,
	A9_RT_FAULT,
};

struct a9_rt_info {
	u64 refused_pa;
	enum a9_rt_reason reason;
	u32 pages;
	u32 ram_pages;
	u32 system_ram_pages;
	u32 reserved_pages;
	u32 nomap_pages;
	u32 linear_pages;
};

struct a9_rt_sample {
	u64 ns;
	int error;
	struct a9_rt_info info;
	u8 bytes[A9_RT_SIZE_MAX];
};

struct a9_rt_target {
	struct a9_aop_ring_pa request;
	struct a9_rt_sample sample[2];
	int error;
	u32 changed_bytes;
	u32 changed_lines;
};

struct a9_rt_capture {
	bool ran;
	int error;
	u32 count;
	u32 bad_count;
	u64 bad_pa[A9_RT_BAD_MAX];
	struct a9_rt_target target[A9_AOP_RING_PA_MAX];
};

struct dentry;

void a9_aop_ring_pa_capture(void);
void a9_aop_ring_pa_debugfs(struct dentry *dir);
bool a9_aop_ring_pa_own_page(u64 pa);

#endif
