// SPDX-License-Identifier: GPL-2.0-only
/* Apple Twister WFI accounting. No PSCI or unverified retention protocol. */
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <asm/cpuidle.h>

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Register Twister WFI accounting (default: off)");

static int __cpuidle a9_enter_wfi(struct cpuidle_device *dev,
				struct cpuidle_driver *drv, int index)
{
	cpu_do_idle();
	return index;
}

static struct cpuidle_driver a9_idle_driver = {
	.name = "apple_a9_wfi",
	.owner = THIS_MODULE,
	.states[0] = {
		.enter = a9_enter_wfi,
		.enter_s2idle = a9_enter_wfi,
		.exit_latency = 1,
		.target_residency = 1,
		.power_usage = UINT_MAX,
		.name = "WFI",
		.desc = "WFI (boot firmware mode)",
	},
	.safe_state_index = 0,
	.state_count = 1,
};

static int __init a9_idle_init(void)
{
	struct device_node *cpu;

	if (!enable || !(of_machine_is_compatible("apple,s8000") ||
			 of_machine_is_compatible("apple,s8003")))
		return -ENODEV;
	for_each_of_cpu_node(cpu) {
		if (!of_device_is_compatible(cpu, "apple,twister")) {
			of_node_put(cpu);
			return -ENODEV;
		}
	}
	/* Deliberately do not change CYC_OVRD or advertise a deep state. */
	return cpuidle_register(&a9_idle_driver, NULL);
}

device_initcall(a9_idle_init);
MODULE_DESCRIPTION("Apple A9 WFI residency accounting");
MODULE_LICENSE("GPL");
