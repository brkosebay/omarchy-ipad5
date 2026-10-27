/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VXD_HOST_LINUX_KERNEL_H
#define VXD_HOST_LINUX_KERNEL_H
#include <linux/types.h>
#include <stdlib.h>	/* before the abs() macro below */
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ALIGN(x, a)		(((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)	(((x) & ((__typeof__(x))(a) - 1)) == 0)
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define min(a, b)		((a) < (b) ? (a) : (b))
#define abs(x)			((x) < 0 ? -(x) : (x))
#define swap(a, b)		do { __typeof__(a) __t = (a); (a) = (b); (b) = __t; } while (0)
#endif
