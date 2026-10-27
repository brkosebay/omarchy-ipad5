/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VXD_HOST_LINUX_BITS_H
#define VXD_HOST_LINUX_BITS_H
#define BIT(n)		(1UL << (n))
#define GENMASK(h, l)	(((~0UL) << (l)) & (~0UL >> (63 - (h))))
#endif
