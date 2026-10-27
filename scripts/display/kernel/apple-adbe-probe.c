// SPDX-License-Identifier: GPL-2.0-only
/*
 * A9 iBoot display observer and opt-in, reversible direct-store experiment.
 * This is deliberately not a DRM driver: simpledrm retains its aperture.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "apple-adbe-probe.h"

#define ADBE_WINDOWS	6
#define ADP_INTEN	0x1028
#define ADP_STATUS	0x102c
#define ADP_FRAME	0x1034
#define ADP_VALID	0x1038
#define ADP_FORMAT	0x402c
#define ADP_BASE		0x4030
#define ADP_STRIDE	0x4038
#define ADP_LAYERS	0x4068
#define ADP_SIZE		0x407c
#define ADP_ERRORS	0xa42
#define ADBE_IRQ_LIMIT	1024

static int irq_index = -1;
module_param(irq_index, int, 0400);
MODULE_PARM_DESC(irq_index, "Phase B: one DT IRQ index 0..4 for 1 s; -1 disables");

static unsigned long mapped_bytes;
module_param(mapped_bytes, ulong, 0400);
MODULE_PARM_DESC(mapped_bytes,
		 "Operator-verified existing DART span from boot FB; 0 disables D2");

static unsigned long scratch_offset = 1536 * 2048 * 4;
module_param(scratch_offset, ulong, 0400);
MODULE_PARM_DESC(scratch_offset, "Scratch offset in reserved boot VRAM, 64-byte aligned");

struct adbe_sample {
	u32 status_or;
	u32 frames;
	u32 changes;
	u32 vbi_samples;
	u32 packet_samples;
	u32 base;
	u16 first;
	u16 last;
};

struct adbe_probe {
	struct device *dev;
	void __iomem *window[ADBE_WINDOWS];
	resource_size_t size[ADBE_WINDOWS];
	void __iomem *adp;
	int adp_index;
	struct resource fb;
	struct resource vram;
	u32 width;
	u32 height;
	u32 stride;
	size_t bytes;
	struct mutex lock; /* Serializes flip, restore, state reads and teardown. */
	struct delayed_work revert_work;
	struct notifier_block reboot_nb;
	struct dentry *debugfs;
	bool stopping;
	bool flipped;
	bool scratch_dirty;
	bool used;
	bool restored;
	u32 saved_base;
	u32 test_base;
	void __iomem *scratch;
	void *scratch_saved;
	struct adbe_sample d1;
	struct adbe_sample d2;
	atomic_t irq_hits;
	atomic_t irq_status;
	atomic_t irq_storm;
	int irq;
};

static u32 adbe_read(struct adbe_probe *p, u32 reg)
{
	return readl(p->adp + reg);
}

/* No ack here: status is a normal, non-destructive read. */
static void adbe_sample(struct adbe_probe *p, unsigned int milliseconds,
			struct adbe_sample *s)
{
	unsigned long end = jiffies + msecs_to_jiffies(milliseconds);
	u16 previous;
	u32 status;

	memset(s, 0, sizeof(*s));
	previous = adbe_read(p, ADP_FRAME);
	s->first = previous;
	do {
		u16 frame;

		usleep_range(1000, 1500);
		frame = adbe_read(p, ADP_FRAME);
		status = adbe_read(p, ADP_STATUS);
		s->frames += (u16)(frame - previous);
		s->changes += frame != previous;
		s->vbi_samples += !!(status & BIT(0));
		s->packet_samples += !!(status & BIT(3));
		s->status_or |= status;
		previous = frame;
	} while (time_before(jiffies, end));
	s->last = previous;
	s->base = adbe_read(p, ADP_BASE);
}

static bool adbe_format(u32 format)
{
	u32 flags = BIT(0) | BIT(21) | BIT(22) | GENMASK(27, 24);

	format &= ~flags;
	return format == 0xc6000 || format == 0xe4000;
}

static int adbe_discriminate(struct adbe_probe *p)
{
	u16 frames[ADBE_WINDOWS] = {};
	u32 formats[ADBE_WINDOWS] = {};
	unsigned int found = 0;
	int i;

	for (i = 0; i < ADBE_WINDOWS; i++) {
		if (p->size[i] < 0x60b0)
			continue;
		formats[i] = readl(p->window[i] + ADP_FORMAT);
		frames[i] = readl(p->window[i] + ADP_FRAME);
		dev_info(p->dev, "window %d fmt=%#x base=%#x stride=%#x layers=%#x frame=%u\n",
			 i, formats[i], readl(p->window[i] + ADP_BASE),
			 readl(p->window[i] + ADP_STRIDE),
			 readl(p->window[i] + ADP_LAYERS), frames[i]);
	}
	msleep(100);
	for (i = 0; i < ADBE_WINDOWS; i++) {
		if (p->size[i] < 0x60b0 || !adbe_format(formats[i]) ||
		    (u16)readl(p->window[i] + ADP_FRAME) == frames[i])
			continue;
		p->adp = p->window[i];
		p->adp_index = i;
		found++;
	}
	if (found != 1)
		return dev_err_probe(p->dev, -ENODEV,
				     "need one format + advancing-counter match, got %u\n", found);
	return 0;
}

static int adbe_framebuffer(struct adbe_probe *p)
{
	struct device_node *np;
	const char *format;
	int ret = -ENODEV;

	for_each_compatible_node(np, NULL, "simple-framebuffer") {
		if (!of_device_is_available(np))
			continue;
		/* Match simpledrm's preference for memory-region over reg. */
		ret = of_reserved_mem_region_to_resource(np, 0, &p->fb);
		if (ret)
			ret = of_address_to_resource(np, 0, &p->fb);
		if (ret)
			break;
		ret = of_property_read_u32(np, "width", &p->width);
		ret |= of_property_read_u32(np, "height", &p->height);
		ret |= of_property_read_u32(np, "stride", &p->stride);
		ret |= of_property_read_string(np, "format", &format);
		if (ret || p->width != 1536 || p->height != 2048 ||
		    p->stride != 6144 || strcmp(format, "x8r8g8b8")) {
			ret = -EINVAL;
			break;
		}
		p->bytes = p->stride * p->height;
		if (resource_size(&p->fb) < p->bytes) {
			ret = -EINVAL;
			break;
		}
		ret = 0;
		break;
	}
	of_node_put(np);
	if (ret)
		return dev_err_probe(p->dev, ret, "need the live 1536x2048 simple-framebuffer\n");
	/* Optional scratch reservation; never allocate or attach an IOMMU. */
	ret = of_reserved_mem_region_to_resource(p->dev->of_node, 0, &p->vram);
	if (ret)
		p->vram = p->fb;
	return 0;
}

static int adbe_mode(struct adbe_probe *p)
{
	u32 base = adbe_read(p, ADP_BASE);
	u32 stride = adbe_read(p, ADP_STRIDE) & ~0x3fU;
	u32 size = adbe_read(p, ADP_SIZE);
	u32 width = size & 0x7fff;
	u32 height = (size >> 16) & 0x7fff;

	dev_info(p->dev, "ADP window %d: raw-base=%#x boot-CPU=%pa stride=%u size-fields=%ux%u layers=%#x fmt=%#x\n",
		 p->adp_index, base, &p->fb.start, stride, width, height,
		 adbe_read(p, ADP_LAYERS) & 0xf, adbe_read(p, ADP_FORMAT));
	/* The RE notes disagree about the order of the two size fields. */
	if (!((width == p->width && height == p->height) ||
	      (height == p->width && width == p->height)) ||
	    stride != p->stride || (base & ~0x3fU) != lower_32_bits(p->fb.start) ||
	    (adbe_read(p, ADP_LAYERS) & 0xf) != 1)
		return dev_err_probe(p->dev, -ENODEV,
				     "live plane geometry/base differs; refusing writes\n");
	dev_info(p->dev, "mode %ux%u stride=%u; CPU high bits come from firmware, not the 32-bit plane register\n",
		 p->width, p->height, stride);
	return 0;
}

static void adbe_dump_log(struct adbe_probe *p, u32 start, u32 end)
{
	u32 reg;

	for (reg = start; reg < end; reg += 4) {
		if (reg == 0x100c || reg == 0x1010)
			continue;
		dev_info(p->dev, "%04x: %08x\n", reg, adbe_read(p, reg));
	}
}

static irqreturn_t adbe_irq(int irq, void *data)
{
	struct adbe_probe *p = data;
	u32 status = adbe_read(p, ADP_STATUS);
	int hits = atomic_inc_return(&p->irq_hits);

	if (status) {
		/* Phase B's only MMIO write: exact observed W1C word. */
		writel(status, p->adp + ADP_STATUS);
		atomic_or(status, &p->irq_status);
	}
	/* Count even empty interrupts: a wrong candidate must also be bounded. */
	if (hits >= ADBE_IRQ_LIMIT && !atomic_xchg(&p->irq_storm, 1))
		disable_irq_nosync(irq);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static int adbe_phase_b(struct platform_device *pdev, struct adbe_probe *p)
{
	int ret;

	if (irq_index == -1)
		return 0;
	p->irq = platform_get_irq(pdev, irq_index);
	if (p->irq < 0)
		return p->irq;
	/* Exclusive request: never disable another driver's shared IRQ. */
	ret = request_irq(p->irq, adbe_irq, IRQF_NO_AUTOEN, "apple-adbe-probe", p);
	if (ret)
		return ret;
	enable_irq(p->irq);
	msleep(1000);
	/* Balance exactly once, whether the storm guard or this thread won. */
	if (!atomic_xchg(&p->irq_storm, 1))
		disable_irq(p->irq);
	synchronize_irq(p->irq);
	dev_info(p->dev, "phase B index=%d Linux-IRQ=%d hits=%d status-or=%#x guard=%s (mask unchanged)\n",
		 irq_index, p->irq, atomic_read(&p->irq_hits),
		 atomic_read(&p->irq_status),
		 atomic_read(&p->irq_hits) >= ADBE_IRQ_LIMIT ? "tripped" : "no");
	free_irq(p->irq, p);
	return 0;
}

/* Caller holds lock. Restore only the register we changed, as an exact word. */
static void adbe_revert(struct adbe_probe *p)
{
	if (!p->flipped && !p->scratch_saved)
		return;
	if (p->flipped) {
		writel(p->saved_base, p->adp + ADP_BASE);
		p->restored = adbe_read(p, ADP_BASE) == p->saved_base;
		/* Keep the scratch image valid while the old scanout drains. */
		msleep(100);
	}
	if (p->scratch_dirty) {
		memcpy_toio(p->scratch, p->scratch_saved, p->bytes);
		/* Complete WC writes before discarding the scratch mapping. */
		wmb();
	}
	if (p->scratch)
		iounmap(p->scratch);
	vfree(p->scratch_saved);
	p->scratch = NULL;
	p->scratch_saved = NULL;
	/* A failed readback remains pending for another restore at teardown. */
	p->flipped = !p->restored;
	p->scratch_dirty = false;
	dev_info(p->dev, "D2 restored exact base %#x (readback %s), scratch bytes restored\n",
		 p->saved_base, p->restored ? "matches" : "DIFFERS");
}

static void adbe_revert_work(struct work_struct *work)
{
	struct adbe_probe *p = container_of(to_delayed_work(work),
					  struct adbe_probe, revert_work);

	mutex_lock(&p->lock);
	adbe_revert(p);
	mutex_unlock(&p->lock);
}

static int adbe_flip(struct adbe_probe *p)
{
	phys_addr_t scratch_phys;
	u64 reserved;
	u32 *row;
	u32 base;
	unsigned int x, y;
	int ret;

	if (p->stopping)
		return -ESHUTDOWN;
	if (p->used)
		return -EALREADY;
	if (!mapped_bytes)
		return -EACCES;
	if (adbe_read(p, ADP_FORMAT) != 0xc6000 &&
	    adbe_read(p, ADP_FORMAT) != 0xe4000)
		return -EINVAL;
	if (p->fb.start < p->vram.start || p->fb.end > p->vram.end)
		return -ERANGE;
	reserved = p->vram.end - p->fb.start + 1;
	base = adbe_read(p, ADP_BASE);
	if (!adbe_scratch_word(base, p->bytes, scratch_offset, mapped_bytes,
			       reserved, &p->test_base))
		return -ENOSPC;
	ret = adbe_mode(p);
	if (ret)
		return ret;
	if (!(adbe_read(p, ADP_VALID) & 1) ||
	    (adbe_read(p, ADP_STATUS) & ADP_ERRORS))
		return -EBUSY;
	adbe_sample(p, 100, &p->d2);
	if (!p->d2.frames)
		return -ETIMEDOUT;
	row = kmalloc(p->stride, GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	p->scratch_saved = vmalloc(p->bytes);
	if (!p->scratch_saved) {
		kfree(row);
		return -ENOMEM;
	}
	scratch_phys = p->fb.start + scratch_offset;
	/* WC matches simpledrm; no request_mem_region/aperture handover. */
	p->scratch = ioremap_wc(scratch_phys, p->bytes);
	if (!p->scratch) {
		vfree(p->scratch_saved);
		p->scratch_saved = NULL;
		kfree(row);
		return -ENOMEM;
	}
	memcpy_fromio(p->scratch_saved, p->scratch, p->bytes);
	p->saved_base = base;
	p->restored = true;
	/* Arm before the first mutation, including scratch writes. */
	if (!schedule_delayed_work(&p->revert_work, msecs_to_jiffies(10000))) {
		ret = -EBUSY;
		goto revert;
	}
	p->scratch_dirty = true;
	for (y = 0; y < p->height; y++) {
		static const u32 bars[] = { 0x00ffffff, 0x00ff0000,
					   0x0000ff00, 0x000000ff };

		for (x = 0; x < p->width; x++)
			row[x] = (x < 32 && y < 32) ? 0 : bars[x * 4 / p->width];
		memcpy_toio(p->scratch + y * p->stride, row, p->stride);
	}
	/* Publish all WC pattern writes before changing the scanout base. */
	wmb();
	if (adbe_read(p, ADP_BASE) != base ||
	    (adbe_read(p, ADP_STATUS) & ADP_ERRORS)) {
		ret = -EIO;
		goto revert;
	}
	p->used = true;
	p->flipped = true;
	/* The single experimental control store; no FIFO, mask or geometry writes. */
	writel(p->test_base, p->adp + ADP_BASE);
	adbe_sample(p, 200, &p->d2);
	dev_info(p->dev, "D2 base=%#x readback=%#x frames=%u status-or=%#x packet-samples=%u (sticky, not proof of completion)\n",
		 p->test_base, p->d2.base, p->d2.frames, p->d2.status_or,
		 p->d2.packet_samples);
	if (p->d2.base != p->test_base || !p->d2.frames ||
	    (p->d2.status_or & ADP_ERRORS)) {
		ret = -EIO;
		goto revert;
	}
	kfree(row);
	return 0;
revert:
	cancel_delayed_work(&p->revert_work);
	adbe_revert(p);
	kfree(row);
	return ret;
}

static int adbe_flip_set(void *data, u64 value)
{
	struct adbe_probe *p = data;
	int ret = 0;

	if (value > 1)
		return -EINVAL;
	mutex_lock(&p->lock);
	if (value) {
		ret = adbe_flip(p);
	} else {
		cancel_delayed_work(&p->revert_work);
		adbe_revert(p);
		if (p->flipped)
			ret = -EIO;
	}
	mutex_unlock(&p->lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(adbe_flip_fops, NULL, adbe_flip_set, "%llu\n");

static int adbe_regs_show(struct seq_file *s, void *unused)
{
	struct adbe_probe *p = s->private;
	static const u32 ranges[][2] = {
		{ 0x1000, 0x100c }, { 0x1014, 0x1080 },
		{ 0x4020, 0x40b0 }, { 0x6020, 0x60b0 },
	};
	u32 reg;
	unsigned int i;

	/* Skip FIFO header/data ports and unknown CLCD apertures. */
	seq_printf(s, "ADP window %d (all reads; no W1C ack)\n", p->adp_index);
	for (i = 0; i < ARRAY_SIZE(ranges); i++)
		for (reg = ranges[i][0]; reg < ranges[i][1]; reg += 4)
			seq_printf(s, "%04x %08x\n", reg, adbe_read(p, reg));
	for (reg = 0; reg < p->size[5]; reg += 4)
		seq_printf(s, "side+%02x %08x\n", reg, readl(p->window[5] + reg));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(adbe_regs);

static void adbe_sample_show(struct seq_file *s, const char *name,
			     struct adbe_sample *sample)
{
	seq_printf(s, "%s frames=%u changes=%u first=%u last=%u vbi-samples=%u packet-samples=%u status-or=%#x base=%#x\n",
		   name, sample->frames, sample->changes, sample->first, sample->last,
		   sample->vbi_samples, sample->packet_samples, sample->status_or, sample->base);
}

static int adbe_state_show(struct seq_file *s, void *unused)
{
	struct adbe_probe *p = s->private;

	mutex_lock(&p->lock);
	seq_printf(s, "window=%d mode=%ux%u stride=%u CPU-fb=%pa fb-bytes=%llu reserved=%pa..%pa\n",
		   p->adp_index, p->width, p->height, p->stride, &p->fb.start,
		   (unsigned long long)resource_size(&p->fb), &p->vram.start, &p->vram.end);
	seq_printf(s, "mapped-bytes=%lu scratch-offset=%lu used=%u flipped=%u restored=%u saved-base=%#x test-base=%#x\n",
		   mapped_bytes, scratch_offset, p->used, p->flipped, p->restored,
		   p->saved_base, p->test_base);
	adbe_sample_show(s, "D1-1s", &p->d1);
	adbe_sample_show(s, "D2-200ms", &p->d2);
	seq_printf(s, "current frame=%u status=%#x mask=%#x base=%#x IRQ-index=%d hits=%d status-or=%#x\n",
		   adbe_read(p, ADP_FRAME) & 0xffff, adbe_read(p, ADP_STATUS),
		   adbe_read(p, ADP_INTEN), adbe_read(p, ADP_BASE), irq_index,
		   atomic_read(&p->irq_hits), atomic_read(&p->irq_status));
	mutex_unlock(&p->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(adbe_state);

static int adbe_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	struct adbe_probe *p = container_of(nb, struct adbe_probe, reboot_nb);

	mutex_lock(&p->lock);
	p->stopping = true;
	adbe_revert(p);
	mutex_unlock(&p->lock);
	return NOTIFY_DONE;
}

static void adbe_quiesce(struct adbe_probe *p)
{
	mutex_lock(&p->lock);
	p->stopping = true;
	mutex_unlock(&p->lock);
	cancel_delayed_work_sync(&p->revert_work);
	mutex_lock(&p->lock);
	adbe_revert(p);
	mutex_unlock(&p->lock);
}

static int adbe_suspend(struct device *dev)
{
	adbe_quiesce(dev_get_drvdata(dev));
	return 0;
}

static int adbe_resume(struct device *dev)
{
	struct adbe_probe *p = dev_get_drvdata(dev);

	mutex_lock(&p->lock);
	p->stopping = false;
	mutex_unlock(&p->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(adbe_pm_ops, adbe_suspend, adbe_resume);

static int adbe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adbe_probe *p;
	struct resource *res;
	int i, ret;

	if (irq_index < -1 || irq_index >= 5)
		return -EINVAL;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	mutex_init(&p->lock);
	INIT_DELAYED_WORK(&p->revert_work, adbe_revert_work);
	platform_set_drvdata(pdev, p);
	for (i = 0; i < ADBE_WINDOWS; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res || resource_size(res) < (i == 5 ? 0x1c : 0x8000))
			return -EINVAL;
		p->size[i] = resource_size(res);
		p->window[i] = devm_ioremap(dev, res->start, p->size[i]);
		if (!p->window[i])
			return -ENOMEM;
	}
	ret = adbe_discriminate(p);
	if (ret)
		return ret;
	ret = adbe_framebuffer(p);
	if (ret)
		return ret;
	ret = adbe_mode(p);
	if (ret)
		return ret;
	adbe_dump_log(p, 0x1000, 0x1080);
	adbe_dump_log(p, 0x4020, 0x40b0);
	adbe_dump_log(p, 0x6020, 0x60b0);
	adbe_sample(p, 1000, &p->d1);
	dev_info(dev, "D1 1s: frames=%u changes=%u VBI-samples=%u packet-samples=%u status-or=%#x\n",
		 p->d1.frames, p->d1.changes, p->d1.vbi_samples,
		 p->d1.packet_samples, p->d1.status_or);
	if (!p->d1.frames)
		return -ENODEV;
	ret = adbe_phase_b(pdev, p);
	if (ret)
		return dev_err_probe(dev, ret, "phase B failed\n");
	p->reboot_nb.notifier_call = adbe_reboot;
	ret = register_reboot_notifier(&p->reboot_nb);
	if (ret)
		return ret;
	p->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(p->debugfs)) {
		ret = PTR_ERR(p->debugfs);
		unregister_reboot_notifier(&p->reboot_nb);
		return ret;
	}
	debugfs_create_file("regs", 0400, p->debugfs, p, &adbe_regs_fops);
	debugfs_create_file("state", 0400, p->debugfs, p, &adbe_state_fops);
	debugfs_create_file("flip", 0200, p->debugfs, p, &adbe_flip_fops);
	dev_info(dev, "ready: /sys/kernel/debug/%s; IRQ and D2 disabled by default\n",
		 dev_name(dev));
	return 0;
}

static void adbe_remove(struct platform_device *pdev)
{
	struct adbe_probe *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->debugfs);
	unregister_reboot_notifier(&p->reboot_nb);
	adbe_quiesce(p);
}

static void adbe_shutdown(struct platform_device *pdev)
{
	adbe_quiesce(platform_get_drvdata(pdev));
}

static const struct of_device_id adbe_of_match[] = {
	{ .compatible = "adbe0,s8000" },
	{}
};
MODULE_DEVICE_TABLE(of, adbe_of_match);

static struct platform_driver adbe_driver = {
	.probe = adbe_probe,
	.remove = adbe_remove,
	.shutdown = adbe_shutdown,
	.driver = {
		.name = "apple-adbe-probe",
		.of_match_table = adbe_of_match,
		.pm = pm_sleep_ptr(&adbe_pm_ops),
	},
};
module_platform_driver(adbe_driver);

MODULE_DESCRIPTION("Read-only A9 iBoot display probe with opt-in reversible flip");
MODULE_AUTHOR("linux-a9 contributors");
MODULE_LICENSE("GPL");
