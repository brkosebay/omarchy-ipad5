/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _A9_AOP_IIO_H
#define _A9_AOP_IIO_H

#include <linux/types.h>

struct device;
struct iio_dev;
struct a9_spu_subpacket;

/* Called by the active driver once the accel stream delivered a report. */
struct a9_aop_accel_config {
	/* Report interval the firmware acknowledged (key 0), microseconds. */
	u32 report_interval_us;
	/* First 0xc2 report received after the rate was acknowledged. */
	const struct a9_spu_subpacket *first_report;
	/*
	 * ARot (key 0x18) was sent: axes are already in the firmware's
	 * device frame, and the DT mount-matrix is the residual. Never apply
	 * ARot again on the AP.
	 */
	bool firmware_applied_arot;
};

struct iio_dev *a9_aop_accel_register(struct device *parent,
				      const struct a9_aop_accel_config *config);
int a9_aop_accel_report(struct iio_dev *indio_dev,
			const struct a9_spu_subpacket *sub);
void a9_aop_accel_invalidate(struct iio_dev *indio_dev);

#endif
