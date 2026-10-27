/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_PMGR_OBSERVE_H
#define _LINUX_SOC_APPLE_PMGR_OBSERVE_H

#include <linux/types.h>

struct device_node;

struct apple_pmgr_observation {
	u32 raw;
	int genpd_status;
};

/*
 * Diagnostic only. Nodes must be in child-before-parent lock order, with no
 * duplicates (maximum five). Takes no power/runtime-PM reference. read() must
 * not sleep or acquire genpd locks; it runs with IRQs disabled and all domain
 * locks held, only if every genpd and hardware actual/target state is ON and
 * reset/disable/parent-off are clear. Hardware auto-PM remains untouched and is
 * recorded in raw; locks exclude software transitions, not autonomous gating.
 * -EHOSTDOWN means read() was skipped.
 */
int apple_pmgr_observe(struct device_node **nodes, unsigned int count,
		       struct apple_pmgr_observation *states,
		       void (*read)(void *), void *data);

#endif
