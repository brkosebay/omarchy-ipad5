// SPDX-License-Identifier: GPL-2.0-only
/* J71t AOP passive bring-up. Deliberately no FIFO, IRQ, reset or write path. */
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/pmgr-observe.h>

#include "a9-aop.h"
#include "a9-aop-ring-reader.h"

#define AOP_REGS 5
#define AOP_DOMAINS 5
#define AOP_MAIN_FW "apple/aop/ipad6faop-20H350.bin"
#define AOP_EXT_FW "apple/aop/ipad6faop_ext-20H350.bin"

static bool debug;
module_param(debug, bool, 0400);
MODULE_PARM_DESC(debug, "Enable read-only J71t AOP debugfs (default off)");
static bool state_probe;
module_param(state_probe, bool, 0400);
MODULE_PARM_DESC(state_probe, "Capture AOP power and raw mailbox state once at probe, PMGR guarded (default off)");
static unsigned int attach_stage;
module_param(attach_stage, uint, 0400);
MODULE_PARM_DESC(attach_stage, "Attach experiment: 1=status, 2=HELLO only, 3=reserved AFK/accel (default 0)");
static bool attach;
module_param(attach, bool, 0400);
MODULE_PARM_DESC(attach, "Allow running-AOP handshake with active=1; observer stays read-only (default off)");
static bool ring_scan;
module_param(ring_scan, bool, 0400);
MODULE_PARM_DESC(ring_scan, "Enable one-shot bounded read-only RAM ring_scan (default off)");
static bool ring_scan_early;
module_param(ring_scan_early, bool, 0400);
MODULE_PARM_DESC(ring_scan_early, "Capture ring_scan at postcore initcall, before device probing (requires debug=1 ring_scan=1)");
static bool ring_scan_reserved;
module_param(ring_scan_reserved, bool, 0400);
MODULE_PARM_DESC(ring_scan_reserved, "Also read reserved linear-mapped System RAM in E1 (requires debug=1 ring_scan=1)");
static unsigned long long ring_pa[A9_AOP_RING_PA_MAX];
static unsigned int ring_pa_nr;
module_param_array(ring_pa, ullong, &ring_pa_nr, 0400);
MODULE_PARM_DESC(ring_pa, "Up to four comma-separated AFK buffer PAs; capture at postcore with debug=1");
static unsigned long long ring_pa_size[A9_AOP_RING_PA_MAX];
static unsigned int ring_pa_size_nr;
module_param_array(ring_pa_size, ullong, &ring_pa_size_nr, 0400);
MODULE_PARM_DESC(ring_pa_size, "Matching comma-separated byte sizes for ring_pa, each 1..65536");
static bool ring_iio;
module_param(ring_iio, bool, 0400);
MODULE_PARM_DESC(ring_iio, "Read accel from early DT-reserved inherited ring, no traffic (default off)");

bool apple_a9_aop_ring_mode(void)
{
	return ring_scan || ring_iio || ring_pa_nr || ring_pa_size_nr;
}

bool apple_a9_aop_state_probe(void)
{
	return state_probe;
}

unsigned int apple_a9_aop_attach_stage(void)
{
	return attach_stage;
}

bool apple_a9_aop_ring_scan_early(void)
{
	return debug && ring_scan && ring_scan_early;
}

bool apple_a9_aop_ring_scan_reserved(void)
{
	return debug && ring_scan && ring_scan_reserved;
}

bool apple_a9_aop_ring_pa_enabled(void)
{
	return debug && (ring_pa_nr || ring_pa_size_nr);
}

int apple_a9_aop_ring_pa_args(struct a9_aop_ring_pa *args, unsigned int max)
{
	unsigned int i;

	if (!ring_pa_nr || ring_pa_nr != ring_pa_size_nr || ring_pa_nr > max)
		return -EINVAL;
	for (i = 0; i < ring_pa_nr; i++) {
		args[i].pa = ring_pa[i];
		args[i].size = ring_pa_size[i];
	}
	return ring_pa_nr;
}

/* Lets the active driver gate its debugfs on the same immutable parameter. */
bool apple_a9_aop_debug(void)
{
	return debug;
}

bool apple_a9_aop_attach(void)
{
	return attach;
}

static const char * const reg_names[AOP_REGS] = {
	"cpu_control", "wrapper_b88", "inbox_control", "outbox_control", "idle_status",
};

static const resource_size_t reg_addresses[AOP_REGS] = {
	0x210800044, 0x210800b88, 0x210804008, 0x210804020, 0x210804040,
};

/* Child -> parent lock order matches s800-0-3-pmgr.dtsi. */
static const char * const domain_names[AOP_DOMAINS] = {
	"aop_gpio", "aop", "aop_busif", "aop_cpu", "aop_filter",
};

struct a9_aop {
	struct device *dev;
	void __iomem *regs[AOP_REGS];
	struct device_node *domains[AOP_DOMAINS];
	char state_line[192];
	char attach_line[512];
};

struct a9_aop_snapshot {
	struct a9_aop *aop;
	u32 values[AOP_REGS];
};

/* Called only inside the existing five-domain PMGR guard. No FIFO reads. */
static void a9_aop_read_state(void *data)
{
	struct a9_aop_snapshot *snapshot = data;

	/* +b88 mirrors inbox on J71t; never classify CPU execution from it. */
	snapshot->values[1] = readl(snapshot->aop->regs[1]);
	snapshot->values[3] = readl(snapshot->aop->regs[3]);
}

static void a9_aop_state_format(char *line, size_t size, u32 pmgr, u32 wrapper_b88,
				u32 outbox, int guard_errno)
{
	u32 target = pmgr & 15, actual = (pmgr >> 4) & 15;

	if (guard_errno && guard_errno != -EHOSTDOWN) {
		scnprintf(line, size,
			  "aop state: pmgr target=unknown actual=unknown wrapper_b88=skipped b88=skipped 4020=skipped -> unknown guard_errno=%d\n",
			  guard_errno);
	} else if (guard_errno) {
		scnprintf(line, size,
			  "aop state: pmgr target=0x%x actual=0x%x wrapper_b88=skipped b88=skipped 4020=skipped -> %s guard_errno=%d\n",
			  target, actual, actual == 0 ? "unpowered" : "unknown", guard_errno);
	} else {
		scnprintf(line, size,
			  "aop state: pmgr target=0x%x actual=0x%x wrapper_b88=0x%08x b88=%u 4020=%u -> powered execution=unknown\n",
			  target, actual, wrapper_b88, wrapper_b88 & 1, outbox & 1);
	}
}

static int a9_aop_capture_state(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_snapshot snapshot = { .aop = aop };
	int ret;

	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_state, &snapshot);
	if (ret == -EPROBE_DEFER)
		return ret;
	/* observe-domains[1] is label-checked ps_aop; all reads must succeed. */
	a9_aop_state_format(aop->state_line, sizeof(aop->state_line), states[1].raw,
			    snapshot.values[1], snapshot.values[3], ret);
	return 0;
}

static int a9_aop_state_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;

	seq_puts(s, aop->state_line);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_state);

/* Status only. Reading FIFO data would consume a message, so cannot be stage 1. */
static void a9_aop_read_attach_status(void *data)
{
	struct a9_aop_snapshot *snapshot = data;
	unsigned int i;

	for (i = 1; i < AOP_REGS; i++)
		snapshot->values[i] = readl(snapshot->aop->regs[i]);
}

static int a9_aop_capture_attach(struct a9_aop *aop)
{
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_snapshot snapshot = { .aop = aop };
	u32 *v = snapshot.values;
	const char *result;
	int ret;

	if (!attach_stage || attach_stage > 3)
		return -EINVAL;
	/*
	 * A duplicate HELLO_REPLY, endpoint restart or address publication does
	 * not prove that a surviving master's old DMA references are gone.
	 * Stages 2/3 belong to the separately compiled active driver. This
	 * observer never maps or accesses hardware for those experiments.
	 */
	if (attach_stage > 1) {
		scnprintf(aop->attach_line, sizeof(aop->attach_line),
			  "format=1 stage=%u result=delegated-to-active-experiment wrapper=skipped observer_hardware_writes=none observer_fifo_reads=none observer_irqs=none observer_dma=none\n",
			  attach_stage);
		return 0;
	}
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_attach_status, &snapshot);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret) {
		scnprintf(aop->attach_line, sizeof(aop->attach_line),
			  "format=1 stage=1 result=aborted-domain-guard guard_errno=%d wrapper=skipped hardware_writes=none fifo_reads=none irqs=none dma=none\n",
			  ret);
		return 0;
	}
	if ((v[2] | v[3]) & (BIT(18) | BIT(19)))
		result = "aborted-mailbox-fault";
	else if (!(v[3] & 1))
		result = "aborted-outbox-disabled";
	else if (!(v[3] & BIT(17)))
		result = "observed-pending-unconsumed";
	else
		result = "observed-mailbox-enabled-empty";
	scnprintf(aop->attach_line, sizeof(aop->attach_line),
		  "format=1 stage=1 result=%s guard_errno=0 execution=unknown wrapper_b88=0x%08x inbox=0x%08x outbox=0x%08x idle=0x%08x hardware_writes=none fifo_reads=none irqs=none dma=none\n",
		  result, v[1], v[2], v[3], v[4]);
	return 0;
}

static int a9_aop_attach_status_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;

	seq_puts(s, aop->attach_line);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_attach_status);

static void a9_aop_read_status(void *data)
{
	struct a9_aop_snapshot *snapshot = data;
	unsigned int i;

	for (i = 0; i < AOP_REGS; i++)
		snapshot->values[i] = readl(snapshot->aop->regs[i]);
}

static int a9_aop_status_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	struct apple_pmgr_observation states[AOP_DOMAINS];
	struct a9_aop_snapshot snapshot = { .aop = aop };
	unsigned int i;
	int ret;

	for (i = 0; i < AOP_DOMAINS; i++) {
		states[i].raw = 0;
		states[i].genpd_status = -1;
	}
	seq_printf(s, "format=1 boottime_ns=%llu hardware_writes=none\n", ktime_get_boottime_ns());
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_status, &snapshot);
	for (i = 0; i < AOP_DOMAINS; i++)
		seq_printf(s, "domain=%s genpd=%d raw=0x%08x actual=%u target=%u auto=%u\n",
			   domain_names[i], states[i].genpd_status, states[i].raw,
			   (states[i].raw >> 4) & 15, states[i].raw & 15,
			   !!(states[i].raw & BIT(28)));
	seq_printf(s, "snapshot_errno=%d\n", ret);
	if (ret) {
		seq_puts(s, "wrapper=skipped reason=domain-off-transition-reset-or-unavailable\n");
		return 0;
	}
	for (i = 0; i < AOP_REGS; i++)
		seq_printf(s, "%s address=0x%llx value=0x%08x\n", reg_names[i],
			   (unsigned long long)reg_addresses[i], snapshot.values[i]);
	seq_printf(s, "wrapper_b88_bit0=%u execution=unknown\n", snapshot.values[1] & 1);
	seq_printf(s, "inbox_faults=%u outbox_faults=%u\n",
		   (snapshot.values[2] >> 18) & 3, (snapshot.values[3] >> 18) & 3);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_status);

static int a9_aop_preflight_show(struct seq_file *s, void *unused)
{
	struct a9_aop *aop = s->private;
	struct apple_pmgr_observation states[AOP_DOMAINS] = {};
	struct a9_aop_snapshot snapshot = { .aop = aop };
	const struct firmware *fw;
	static const char * const names[] = { AOP_MAIN_FW, AOP_EXT_FW };
	static const size_t sizes[] = { 342336, 37024 };
	unsigned int i;
	int ret;

	seq_puts(s, "hardware_writes=none fifo_reads=none irqs=none dma=none\n");
	if (!attach) {
		seq_puts(s, "result=disabled enable=apple_a9_aop.attach=1\n");
		return 0;
	}
	ret = apple_pmgr_observe(aop->domains, AOP_DOMAINS, states,
				 a9_aop_read_status, &snapshot);
	if (ret) {
		seq_printf(s, "result=blocked-domain-state errno=%d\n", ret);
		return 0;
	}
	if ((snapshot.values[2] | snapshot.values[3]) & GENMASK(19, 18)) {
		seq_puts(s, "result=blocked-mailbox-fault action=no-fifo-no-ack-no-reset\n");
		return 0;
	}
	if (snapshot.values[1] & 1) {
		seq_puts(s, "case=b88-set execution=unknown result=blocked-retained-memory-and-host-ownership\n");
		seq_puts(s, "action=retain-firmware-no-hello-no-fifo-no-reset\n");
		return 0;
	}
	seq_puts(s, "case=b88-clear execution=unknown\n");
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = request_firmware_direct(&fw, names[i], aop->dev);
		if (ret) {
			seq_printf(s, "firmware=%s errno=%d result=blocked-firmware\n",
				   names[i], ret);
			return 0;
		}
		seq_printf(s, "firmware=%s size=%zu expected=%zu\n",
			   names[i], fw->size, sizes[i]);
		ret = fw->size == sizes[i] ? 0 : -EINVAL;
		release_firmware(fw);
		if (ret) {
			seq_puts(s, "result=blocked-firmware-size\n");
			return 0;
		}
	}
	seq_puts(s, "result=blocked-memory-reservation-remap-and-patchbay\n");
	seq_puts(s, "action=no-load-no-cpu-release-no-mailbox\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_aop_preflight);

static void a9_aop_put_node(void *node)
{
	of_node_put(node);
}

static void a9_aop_remove_debugfs(void *dir)
{
	debugfs_remove_recursive(dir);
}

static int a9_aop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a9_aop *aop;
	struct dentry *dir;
	struct resource *res;
	const char *label;
	unsigned int i;
	int ret;

	/* No resource requests or MMIO on the ordinary boot path. */
	if ((!debug && !state_probe && !attach_stage) ||
	    !of_machine_is_compatible("apple,j71t"))
		return -ENODEV;
	if (attach_stage > 3)
		return dev_err_probe(dev, -EINVAL, "attach_stage must be 0..3\n");
	if (attach_stage && (debug || state_probe || attach || apple_a9_aop_ring_mode()))
		return dev_err_probe(dev, -EBUSY,
				     "attach_stage requires other observer modes off\n");
	if (of_find_property(dev->of_node, "power-domains", NULL) ||
	    platform_get_resource(pdev, IORESOURCE_MEM, AOP_REGS))
		return -EINVAL;
	aop = devm_kzalloc(dev, sizeof(*aop), GFP_KERNEL);
	if (!aop)
		return -ENOMEM;
	aop->dev = dev;
	if (attach_stage > 1) {
		ret = a9_aop_capture_attach(aop);
		if (ret)
			return ret;
		goto debugfs;
	}
	/* Ring-only mode leaves the wrapper untouched; state_probe is independent. */
	if (apple_a9_aop_ring_mode() && !state_probe)
		goto debugfs;
	for (i = 0; i < AOP_REGS; i++) {
		if (attach_stage == 1) {
			if (!i)
				continue;
		} else if ((apple_a9_aop_ring_mode() || !debug) && i != 1 && i != 3) {
			continue;
		}
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res || res->start != reg_addresses[i] || resource_size(res) != 4)
			return -EINVAL;
		aop->regs[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(aop->regs[i]))
			return PTR_ERR(aop->regs[i]);
	}
	if (of_count_phandle_with_args(dev->of_node, "apple,observe-domains", NULL) != AOP_DOMAINS)
		return -EINVAL;
	for (i = 0; i < AOP_DOMAINS; i++) {
		aop->domains[i] = of_parse_phandle(dev->of_node, "apple,observe-domains", i);
		if (!aop->domains[i])
			return -EINVAL;
		ret = devm_add_action_or_reset(dev, a9_aop_put_node, aop->domains[i]);
		if (ret)
			return ret;
		if (of_property_read_string(aop->domains[i], "label", &label) ||
		    strcmp(label, domain_names[i]))
			return -EINVAL;
	}
	if (state_probe) {
		ret = a9_aop_capture_state(aop);
		if (ret)
			return dev_err_probe(dev, ret, "state probe needs PMGR providers\n");
	}
	if (attach_stage) {
		ret = a9_aop_capture_attach(aop);
		if (ret)
			return dev_err_probe(dev, ret, "attach observation needs PMGR providers\n");
	}
debugfs:
	dir = debugfs_create_dir("apple-a9-aop", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	if (debug && apple_a9_aop_ring_mode()) {
		ret = a9_aop_ring_reader_init(dev, dir, ring_scan, ring_iio);
		if (ret) {
			debugfs_remove_recursive(dir);
			return ret;
		}
	} else if (debug) {
		debugfs_create_file("status", 0400, dir, aop, &a9_aop_status_fops);
		debugfs_create_file("preflight", 0400, dir, aop, &a9_aop_preflight_fops);
	}
	if (state_probe)
		debugfs_create_file("state_probe", 0400, dir, aop, &a9_aop_state_fops);
	if (attach_stage)
		debugfs_create_file("attach_status", 0400, dir, aop,
				    &a9_aop_attach_status_fops);
	/* Register last: remove open debugfs users before any private data/maps. */
	ret = devm_add_action_or_reset(dev, a9_aop_remove_debugfs, dir);
	if (ret)
		return ret;
	if (state_probe)
		pr_info("%s", aop->state_line);
	if (attach_stage)
		dev_info(dev, "%s", aop->attach_line);
	if (debug && apple_a9_aop_ring_mode())
		dev_info(dev, "read-only RAM reader ready; scan=%u iio=%u; wrapper=%s mailbox=none\n",
			 ring_scan, ring_iio, state_probe ? "state-probe-only" : "none");
	else if (debug)
		dev_info(dev, "read-only debugfs ready; attach preflight %s; no power consumer\n",
			 attach ? "enabled" : "disabled");
	return 0;
}

static const struct of_device_id a9_aop_match[] = {
	{ .compatible = "apple,j71t-aop-observer" },
	{}
};

static struct platform_driver a9_aop_driver = {
	.probe = a9_aop_probe,
	.driver = {
		.name = "apple-a9-aop",
		.of_match_table = a9_aop_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(a9_aop_driver);

MODULE_FIRMWARE(AOP_MAIN_FW);
MODULE_FIRMWARE(AOP_EXT_FW);
MODULE_DESCRIPTION("Apple J71t AOP read-only bring-up observer");
MODULE_LICENSE("GPL");
