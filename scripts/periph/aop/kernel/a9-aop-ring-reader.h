/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _A9_AOP_RING_READER_H
#define _A9_AOP_RING_READER_H

#include <linux/types.h>

struct device;
struct dentry;

int a9_aop_ring_reader_init(struct device *dev, struct dentry *dir,
			    bool scan, bool iio);

#endif
