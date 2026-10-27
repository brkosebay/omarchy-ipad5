// SPDX-License-Identifier: GPL-2.0-only
/*
 * S8003 on-die sensors, recovered from AppleT700XTempSensor (j71t 20H350).
 * The hardware reports calibrated signed Q6 Celsius. Preserve iBoot's trim,
 * CTL1/2, alarm and SoCHOT configuration; never copy another chip's fuses.
 * See docs/power/overclock-round2.md in the linux-a9 workspace for evidence.
 */
#include <linux/debugfs.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/soc/apple/s8003-power.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define A9_SENSORS 6
#define A9_SAMPLE_MS 100
#define A9_STALE_MS 500
/* integ31 live readings validate these three; retain the others as raw
 * diagnostics until their corrected mapping/calibration is checked on-device.
 */
#define A9_TRUSTED_MASK (BIT(0) | BIT(1) | BIT(5))

struct a9_thermal;
struct a9_sensor {
	struct a9_thermal *parent;
	unsigned int id;
	u32 bk, count, maximum, ctl;
	int temp, error;
	unsigned long sampled;
};

struct a9_thermal {
	void __iomem *base[2];
	struct mutex lock;
	struct delayed_work work;
	struct a9_sensor sensor[A9_SENSORS];
	struct dentry *debugfs;
	bool profile_ready;
};

static const char * const a9_names[A9_SENSORS] = {
	"apple-a9-soc0", "apple-a9-soc1", "apple-a9-soc2",
	"apple-a9-tcc0", "apple-a9-tcc1", "apple-a9-tcc2",
};

static const u16 a9_offsets[A9_SENSORS] = { 0, 0x100, 0x200, 0, 0x40, 0x200 };
static const u16 a9_clear_offsets[A9_SENSORS] = { 4, 0x104, 0x204, 0x408, 0x438, 0x468 };

static bool a9_sensor_trusted(unsigned int id)
{
	return id < A9_SENSORS && (A9_TRUSTED_MASK & BIT(id));
}

static void __iomem *a9_sensor_base(struct a9_sensor *s)
{
	return s->parent->base[s->id / 3] + a9_offsets[s->id];
}

static void a9_sample(struct a9_sensor *s)
{
	void __iomem *base = a9_sensor_base(s);
	bool tcc = s->id >= 3;
	int q6;

	/* iOS timer: freeze accumulation, read, then start the next interval. */
	writel(BIT(3), s->parent->base[s->id / 3] + a9_clear_offsets[s->id]);
	s->ctl = readl(base);
	s->bk = readl(base + (tcc ? 0x20 : 0x18));
	s->count = readl(base + (tcc ? 0x38 : 0x24));
	s->maximum = readl(base + (tcc ? 0x28 : 0x1c));
	writel(BIT(3), base);

	s->error = -EAGAIN;
	if (!(s->ctl & BIT(0)) || !(s->bk & BIT(31)) ||
	    !s->count || (s->count & BIT(31)))
		return;
	q6 = sign_extend32(s->bk, tcc ? 15 : 14);
	s->temp = q6 * 1000 / 64;
	/* Reject impossible codes rather than replacing them with iOS's 30 C. */
	if (s->temp < -40000 || s->temp > 150000) {
		s->error = -ERANGE;
		return;
	}
	s->sampled = jiffies;
	s->error = 0;
}

static void a9_sample_work(struct work_struct *work)
{
	struct a9_thermal *a = container_of(to_delayed_work(work),
					  struct a9_thermal, work);
	int i;

	mutex_lock(&a->lock);
	for (i = 0; i < A9_SENSORS; i++)
		a9_sample(&a->sensor[i]);
	mutex_unlock(&a->lock);
	mod_delayed_work(system_wq, &a->work, msecs_to_jiffies(A9_SAMPLE_MS));
}

static int a9_sensor_temp(struct a9_sensor *s, int *temp)
{
	int ret;

	mutex_lock(&s->parent->lock);
	ret = a9_sensor_trusted(s->id) ? s->error : -ENODATA;
	if (!ret && time_after(jiffies, s->sampled + msecs_to_jiffies(A9_STALE_MS)))
		ret = -ETIMEDOUT;
	if (!ret)
		*temp = s->temp;
	mutex_unlock(&s->parent->lock);
	return ret;
}

static int a9_get_temp(struct thermal_zone_device *tz, int *temp)
{
	return a9_sensor_temp(thermal_zone_device_priv(tz), temp);
}

static void a9_thermal_hot(struct thermal_zone_device *tz)
{
	struct a9_sensor *s = thermal_zone_device_priv(tz);

	if (a9_sensor_trusted(s->id))
		apple_a9_thermal_revert();
}

static void a9_thermal_critical(struct thermal_zone_device *tz)
{
	struct a9_sensor *s = thermal_zone_device_priv(tz);

	if (!a9_sensor_trusted(s->id))
		return;
	/* Synchronous bounded fallback first, then the normal shutdown path. */
	apple_a9_thermal_revert();
	thermal_zone_device_critical(tz);
}

static const struct thermal_zone_device_ops a9_thermal_ops = {
	.get_temp = a9_get_temp,
	.hot = a9_thermal_hot,
	.critical = a9_thermal_critical,
};

struct a9_trip_profile {
	struct thermal_zone_device *tz;
	struct apple_a9_thermal_profile profile;
	unsigned int seen;
	bool apply;
};

/* thermal_zone_for_each_trip holds the zone lock. Use the core setter so
 * trip ordering/thresholds and the existing cpufreq cooling binding stay valid.
 * Validate the unmodified DT first; never raise an unexpected critical trip.
 */
static int a9_profile_trip(struct thermal_trip *trip, void *data)
{
	struct a9_trip_profile *p = data;
	unsigned int bit, stock, selected;

	switch (trip->type) {
	case THERMAL_TRIP_PASSIVE:
		bit = BIT(0);
		stock = 80000;
		selected = p->profile.passive;
		break;
	case THERMAL_TRIP_HOT:
		bit = BIT(1);
		stock = 90000;
		selected = p->profile.hot;
		break;
	case THERMAL_TRIP_CRITICAL:
		bit = BIT(2);
		stock = 100000;
		selected = stock;
		break;
	default:
		return -EINVAL;
	}
	if (!p->apply) {
		if ((p->seen & bit) || trip->temperature != stock)
			return -EINVAL;
		p->seen |= bit;
	} else if (trip->type != THERMAL_TRIP_CRITICAL) {
		thermal_zone_set_trip_temp(p->tz, trip, selected);
	}
	return 0;
}

static int a9_apply_profile(struct thermal_zone_device *tz)
{
	struct a9_trip_profile p = {
		.tz = tz, .profile = apple_a9_get_thermal_profile(),
	};
	int ret;

	if (!p.profile.tuning)
		return 0;
	ret = thermal_zone_for_each_trip(tz, a9_profile_trip, &p);
	if (ret || p.seen != (BIT(0) | BIT(1) | BIT(2)))
		return ret ?: -EINVAL;
	p.apply = true;
	ret = thermal_zone_for_each_trip(tz, a9_profile_trip, &p);
	thermal_zone_device_update(tz, THERMAL_EVENT_UNSPECIFIED);
	return ret;
}

static umode_t a9_hwmon_visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	return type == hwmon_temp ? 0444 : 0;
}

static int a9_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *value)
{
	struct a9_thermal *a = dev_get_drvdata(dev);
	int temp, ret;

	ret = a9_sensor_temp(&a->sensor[channel], &temp);
	if (attr == hwmon_temp_fault) {
		*value = !!ret;
		return 0;
	}
	if (ret)
		return ret;
	*value = temp;
	return 0;
}

static int a9_hwmon_label(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, const char **str)
{
	*str = a9_names[channel];
	return 0;
}

static const struct hwmon_ops a9_hwmon_ops = {
	.is_visible = a9_hwmon_visible,
	.read = a9_hwmon_read,
	.read_string = a9_hwmon_label,
};

#define A9_HWMON (HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_FAULT)
static const struct hwmon_channel_info * const a9_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, A9_HWMON, A9_HWMON, A9_HWMON,
			   A9_HWMON, A9_HWMON, A9_HWMON), NULL,
};
static const struct hwmon_chip_info a9_hwmon_chip = {
	.ops = &a9_hwmon_ops,
	.info = a9_hwmon_info,
};

static int a9_thermal_snapshot_show(struct seq_file *seq, void *unused)
{
	struct a9_thermal *a = seq->private;
	struct apple_a9_thermal_profile p = apple_a9_get_thermal_profile();
	int i;

	seq_printf(seq, "format=3 units=milliC trim=boot-preserved sample_ms=100 trusted_mask=%02x thermal_profile_format=1 profile_ready=%u profile=%s profile_tuning=%u passive_millic=%u hot_millic=%u critical_millic=%u arm_millic=%u cooldown_millic=%u\n",
		   (unsigned int)A9_TRUSTED_MASK, READ_ONCE(a->profile_ready),
		   apple_a9_thermal_profile_name(p), p.tuning, p.passive, p.hot,
		   p.critical, p.arm, p.cooldown);
	mutex_lock(&a->lock);
	for (i = 0; i < A9_SENSORS; i++) {
		struct a9_sensor *s = &a->sensor[i];
		void __iomem *base = a9_sensor_base(s);
		bool tcc = i >= 3;

		seq_printf(seq, "%s bk=%08x count=%08x max=%08x ctl=%08x temp=%d error=%d age_ms=%u trusted=%u ctl_offset=%03x ctl1=%08x ctl2=%08x params=%08x\n",
			   a9_names[i], s->bk, s->count, s->maximum, s->ctl,
			   s->temp, s->error, jiffies_to_msecs(jiffies - s->sampled),
			   a9_sensor_trusted(i), a9_offsets[i], readl(base + 8),
			   readl(base + (tcc ? 16 : 12)), tcc ? readl(base + 24) : 0);
	}
	/* LLB 20H350 table at 0x1803b1af8 initializes these shared TCC
	 * coefficients. The old uniform-stride bug wrote CTL bits into +100.
	 * Read only: a normal LLB boot must restore calibration after integ31.
	 */
	for (i = 0; i < 3; i++) {
		void __iomem *base = a->base[1] + 0x100 + i * 0x18;

		seq_printf(seq, "tcc-coeff%u offset=%03x a=%08x b=%08x c=%08x\n",
			   i, 0x100 + i * 0x18, readl(base), readl(base + 8), readl(base + 16));
	}
	mutex_unlock(&a->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_thermal_snapshot);

static void a9_thermal_stop(void *data)
{
	struct a9_thermal *a = data;

	cancel_delayed_work_sync(&a->work);
	debugfs_remove_recursive(a->debugfs);
	/* Keep sensors and the boot hardware protections running on unload. */
}

static int a9_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a9_thermal *a;
	struct device *hwmon;
	int i, ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	mutex_init(&a->lock);
	for (i = 0; i < 2; i++) {
		a->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(a->base[i]))
			return PTR_ERR(a->base[i]);
	}
	INIT_DELAYED_WORK(&a->work, a9_sample_work);
	for (i = 0; i < A9_SENSORS; i++) {
		a->sensor[i].parent = a;
		a->sensor[i].id = i;
		a->sensor[i].error = -EAGAIN;
		/* iOS start: CTL0_SET=1. Do not alter CTL1/2 or trim params. */
		writel(BIT(0) | BIT(3), a9_sensor_base(&a->sensor[i]));
	}
	a->debugfs = debugfs_create_dir("a9_thermal", NULL);
	debugfs_create_file("snapshot", 0400, a->debugfs, a, &a9_thermal_snapshot_fops);
	ret = devm_add_action_or_reset(dev, a9_thermal_stop, a);
	if (ret)
		return ret;
	mod_delayed_work(system_wq, &a->work, msecs_to_jiffies(A9_SAMPLE_MS));
	for (i = 0; i < A9_SENSORS; i++) {
		struct thermal_zone_device *tz;

		if (!a9_sensor_trusted(i))
			continue;
		tz = devm_thermal_of_zone_register(dev, i, &a->sensor[i], &a9_thermal_ops);
		if (IS_ERR(tz))
			return dev_err_probe(dev, PTR_ERR(tz), "thermal zone %d\n", i);
		ret = a9_apply_profile(tz);
		if (ret)
			return dev_err_probe(dev, ret, "thermal profile zone %d\n", i);
	}
	WRITE_ONCE(a->profile_ready, true);
	hwmon = devm_hwmon_device_register_with_info(dev, "apple_a9", a, &a9_hwmon_chip, NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}

static const struct of_device_id a9_thermal_match[] = {
	{ .compatible = "apple,s8003-thermal" }, {}
};
MODULE_DEVICE_TABLE(of, a9_thermal_match);

static struct platform_driver a9_thermal_driver = {
	.probe = a9_thermal_probe,
	.driver = {
		.name = "apple-s8003-thermal",
		.of_match_table = a9_thermal_match,
	},
};
module_platform_driver(a9_thermal_driver);
MODULE_DESCRIPTION("Apple S8003 validated die thermals and raw channel diagnostics");
MODULE_LICENSE("GPL");
