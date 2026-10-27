// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO accelerometer for the J71t AOP "accel" service. Raw values are the AOP
 * report counts (signed 16.16 g working interpretation, ds-aop-accel.md §8);
 * scale converts them to m/s^2. Direct reads return the latest report and
 * -ENODATA when stale; a triggered buffer is fed by the device's own trigger,
 * fired once per report.
 */
#include <linux/err.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "a9-aop-protocol.h"
#include "a9-aop-iio.h"

struct a9_aop_accel {
	spinlock_t lock; /* Protects latest, received_ns and valid. */
	struct a9_aop_motion latest;
	u64 received_ns;
	u64 stale_ns;
	bool valid;
	u32 interval_us;
	struct iio_mount_matrix matrix;
	struct iio_trigger *trig;
};

static const struct iio_mount_matrix *
a9_aop_accel_matrix(const struct iio_dev *indio_dev, const struct iio_chan_spec *chan)
{
	struct a9_aop_accel *accel = iio_priv(indio_dev);

	return &accel->matrix;
}

static const struct iio_chan_spec_ext_info a9_aop_accel_ext[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, a9_aop_accel_matrix),
	{}
};

#define A9_ACCEL_CHANNEL(axis, index) { \
	.type = IIO_ACCEL, .modified = 1, .channel2 = IIO_MOD_##axis, \
	.address = index, .scan_index = index, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | \
		BIT(IIO_CHAN_INFO_SAMP_FREQ), .ext_info = a9_aop_accel_ext, \
	.scan_type = { .sign = 's', .realbits = 32, .storagebits = 32, \
		       .endianness = IIO_CPU }, \
}

static const struct iio_chan_spec a9_aop_accel_channels[] = {
	A9_ACCEL_CHANNEL(X, 0), A9_ACCEL_CHANNEL(Y, 1), A9_ACCEL_CHANNEL(Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const unsigned long a9_aop_accel_scan_masks[] = { 0x7, 0 };

static int a9_aop_accel_read_raw(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 int *val, int *val2, long mask)
{
	struct a9_aop_accel *accel = iio_priv(indio_dev);
	unsigned long flags;
	u64 now;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		spin_lock_irqsave(&accel->lock, flags);
		now = ktime_get_boottime_ns();
		if (!accel->valid || now - accel->received_ns > accel->stale_ns) {
			ret = -ENODATA;
		} else {
			*val = accel->latest.axes[chan->address];
			ret = IIO_VAL_INT;
		}
		spin_unlock_irqrestore(&accel->lock, flags);
		return ret;
	case IIO_CHAN_INFO_SCALE:
		/* 9.80665 / 65536 m/s^2 per count (16.16 g). */
		*val = 0;
		*val2 = 149638;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = 1000000;
		*val2 = accel->interval_us;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static const struct iio_info a9_aop_accel_info = {
	.read_raw = a9_aop_accel_read_raw,
	.validate_trigger = iio_validate_own_trigger,
};

static const struct iio_trigger_ops a9_aop_accel_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static irqreturn_t a9_aop_accel_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct a9_aop_accel *accel = iio_priv(indio_dev);
	struct {
		s32 axes[3];
		aligned_s64 timestamp;
	} scan = {};
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&accel->lock, flags);
	valid = accel->valid;
	memcpy(scan.axes, accel->latest.axes, sizeof(scan.axes));
	spin_unlock_irqrestore(&accel->lock, flags);
	/* AP receipt time: the AOP timestamp domain is not established. */
	if (valid)
		iio_push_to_buffers_with_ts(indio_dev, &scan, sizeof(scan),
					    iio_get_time_ns(indio_dev));
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* Process context only (fires the trigger synchronously). */
int a9_aop_accel_report(struct iio_dev *indio_dev, const struct a9_spu_subpacket *sub)
{
	struct a9_aop_accel *accel = iio_priv(indio_dev);
	struct a9_aop_motion motion;
	unsigned long flags;
	int ret;

	ret = a9_spu_motion(sub, &motion);
	if (ret) {
		a9_aop_accel_invalidate(indio_dev);
		return ret;
	}
	spin_lock_irqsave(&accel->lock, flags);
	accel->latest = motion;
	/* AP receipt time is used only for freshness and buffer timestamps. */
	accel->received_ns = ktime_get_boottime_ns();
	accel->valid = true;
	spin_unlock_irqrestore(&accel->lock, flags);
	if (iio_buffer_enabled(indio_dev))
		iio_trigger_poll_nested(accel->trig);
	return 0;
}

void a9_aop_accel_invalidate(struct iio_dev *indio_dev)
{
	struct a9_aop_accel *accel = iio_priv(indio_dev);
	unsigned long flags;

	spin_lock_irqsave(&accel->lock, flags);
	accel->valid = false;
	spin_unlock_irqrestore(&accel->lock, flags);
}

struct iio_dev *a9_aop_accel_register(struct device *parent,
				      const struct a9_aop_accel_config *config)
{
	struct a9_aop_accel *accel;
	struct iio_dev *indio_dev;
	int ret;

	if (!config->first_report || !config->report_interval_us ||
	    config->report_interval_us > 10000000)
		return ERR_PTR(-EINVAL);
	indio_dev = devm_iio_device_alloc(parent, sizeof(*accel));
	if (!indio_dev)
		return ERR_PTR(-ENOMEM);
	accel = iio_priv(indio_dev);
	spin_lock_init(&accel->lock);
	accel->interval_us = config->report_interval_us;
	accel->stale_ns = max_t(u64, 1000000000ULL, (u64)accel->interval_us * 5000);
	/*
	 * Residual screen matrix from the board DT after six poses; identity
	 * (the AOP report frame) until measured. ARot is never applied here.
	 */
	ret = iio_read_mount_matrix(parent, &accel->matrix);
	if (ret)
		return ERR_PTR(ret);
	if (!config->firmware_applied_arot)
		dev_warn(parent, "ARot not sent to firmware: axes are the sensor frame, not the DT mount-matrix frame\n");
	indio_dev->name = "apple-a9-aop-accel";
	indio_dev->info = &a9_aop_accel_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = a9_aop_accel_channels;
	indio_dev->num_channels = ARRAY_SIZE(a9_aop_accel_channels);
	indio_dev->available_scan_masks = a9_aop_accel_scan_masks;
	ret = a9_aop_accel_report(indio_dev, config->first_report);
	if (ret)
		return ERR_PTR(ret);

	accel->trig = devm_iio_trigger_alloc(parent, "%s-dev%d", indio_dev->name,
					     iio_device_id(indio_dev));
	if (!accel->trig)
		return ERR_PTR(-ENOMEM);
	accel->trig->ops = &a9_aop_accel_trigger_ops;
	iio_trigger_set_drvdata(accel->trig, indio_dev);
	ret = devm_iio_trigger_register(parent, accel->trig);
	if (ret)
		return ERR_PTR(ret);
	indio_dev->trig = iio_trigger_get(accel->trig);
	ret = devm_iio_triggered_buffer_setup(parent, indio_dev, NULL,
					      a9_aop_accel_trigger_handler, NULL);
	if (ret)
		return ERR_PTR(ret);
	ret = devm_iio_device_register(parent, indio_dev);
	if (ret)
		return ERR_PTR(ret);
	return indio_dev;
}

MODULE_DESCRIPTION("Apple A9 AOP accelerometer IIO consumer");
MODULE_LICENSE("GPL");
