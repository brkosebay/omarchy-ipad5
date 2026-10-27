// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in S8003 observer. No stores to MMIO, PMIC or DVFS control paths.
 * PSINFO offsets: HoolockLinux/m1n1 d5a10ac, src/cpufreq.c;
 * independently read by AppleS8000PMGR in the j71t 20H350 kernelcache.
 * This driver never enables a clock, powers a device or initializes a sensor.
 */
#include <linux/capability.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/seq_file.h>

#define ACC_BASE 0x202220000ULL
static bool enable;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Enable fixed-address read-only S8003 CPU snapshot (default off)");
static void __iomem *acc;
static struct dentry *directory;

static int a9_ro_show(struct seq_file *s, void *unused)
{
	u64 before, after, lo, hi;
	u32 status;
	unsigned int n, off;

	before = readq(acc + 0x20);
	status = readl(acc + 0x50);
	seq_printf(s, "format=1 hardware_writes=none base=%#llx\n", ACC_BASE);
	seq_printf(s, "cmd_before=%016llx busy=%u request1=%u request2=%u status=%08x actual=%u target=%u\n",
		   before, !!(before & BIT_ULL(31)), (unsigned int)(before & 31),
		   (unsigned int)((before >> 12) & 15), status,
		   (status >> 4) & 15, status & 15);
	/* Generic driver offsets are not enough to justify new A9 MMIO probes. */
	seq_puts(s, "pll_status=NOT_READ pll_factor=NOT_READ pll_decode=UNRESOLVED\n");
	seq_puts(s, "state psinfo1_address psinfo1_raw psinfo2_address psinfo2_raw code1_63_56 code2_39_32 max_load_43_40\n");
	for (n = 0; n < 16; n++) {
		off = n < 8 ? 0x68 + n * 8 : 0x368 + (n - 8) * 8;
		lo = readq(acc + off);
		hi = readq(acc + 0x868 + n * 8);
		seq_printf(s, "%u %llx %016llx %llx %016llx %llu %llu %llu\n", n,
			   ACC_BASE + off, lo, ACC_BASE + 0x868 + n * 8, hi,
			   lo >> 56, (hi >> 32) & 255, (hi >> 40) & 15);
	}
	after = readq(acc + 0x20);
	seq_printf(s, "cmd_after=%016llx transition_observed=%u\n", after, before != after);
	seq_puts(s, "frequency_per_state=requires_correlated_OPP_and_PLL_decode rail_uv=UNRESOLVED temperatures=use_validated_thermal_sysfs\n");
	return 0;
}

static int a9_ro_open(struct inode *inode, struct file *file)
{
	if (!enable || !capable(CAP_SYS_RAWIO))
		return -EPERM;
	return single_open(file, a9_ro_show, NULL);
}

static const struct file_operations a9_ro_fops = {
	.owner = THIS_MODULE,
	.open = a9_ro_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init a9_ro_init(void)
{
	struct dentry *file;

	if (!enable)
		return -EPERM;
	if (!of_machine_is_compatible("apple,s8003"))
		return -ENODEV;
	acc = ioremap(ACC_BASE, 0x1000);
	if (!acc)
		return -ENOMEM;
	directory = debugfs_create_dir("a9_dvfs_ro", NULL);
	if (IS_ERR(directory)) {
		iounmap(acc);
		return PTR_ERR(directory);
	}
	file = debugfs_create_file("snapshot", 0400, directory, NULL, &a9_ro_fops);
	if (IS_ERR_OR_NULL(file)) {
		debugfs_remove_recursive(directory);
		iounmap(acc);
		return file ? PTR_ERR(file) : -ENOMEM;
	}
	return 0;
}

static void __exit a9_ro_exit(void)
{
	debugfs_remove_recursive(directory);
	iounmap(acc);
}
module_init(a9_ro_init);
module_exit(a9_ro_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("S8003 CPU DVFS observer with no hardware write path");
