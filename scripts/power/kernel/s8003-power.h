/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_S8003_POWER_H
#define _LINUX_SOC_APPLE_S8003_POWER_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct apple_a9_thermal_profile {
	bool tuning;
	int passive, hot, critical, arm, cooldown;
};

static inline struct apple_a9_thermal_profile apple_a9_stock_thermal_profile(void)
{
	return (struct apple_a9_thermal_profile) {
		.passive = 80000, .hot = 90000, .critical = 100000,
		.arm = 75000, .cooldown = 70000,
	};
}

static inline const char *
apple_a9_thermal_profile_name(struct apple_a9_thermal_profile p)
{
	return !p.tuning ? "stock" : p.passive == 90000 ? "sustained" : "custom";
}

#if IS_ENABLED(CONFIG_APPLE_A9_TUNING)
void apple_a9_thermal_revert(void);
struct apple_a9_thermal_profile apple_a9_get_thermal_profile(void);
#else
static inline void apple_a9_thermal_revert(void) {}
static inline struct apple_a9_thermal_profile apple_a9_get_thermal_profile(void)
{
	return apple_a9_stock_thermal_profile();
}
#endif

#endif
