/* SPDX-License-Identifier: GPL-2.0 */
/* Host shim for drivers/media/v4l2-core/v4l2-h264.c. */
#ifndef VXD_HOST_LINUX_MODULE_H
#define VXD_HOST_LINUX_MODULE_H
#include <linux/kernel.h>
#include <linux/slab.h>
#include <stdio.h>
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define EXPORT_SYMBOL_GPL(x)
#define pr_debug(fmt, ...)	do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define scnprintf		snprintf
#define WARN_ON(c)		(!!(c))
#endif
