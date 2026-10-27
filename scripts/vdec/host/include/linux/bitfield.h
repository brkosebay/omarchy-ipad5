/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VXD_HOST_LINUX_BITFIELD_H
#define VXD_HOST_LINUX_BITFIELD_H
#include <linux/bits.h>
#define __bf_shf(m)		(__builtin_ffsll(m) - 1)
#define FIELD_PREP(m, v)	((((unsigned long)(v)) << __bf_shf(m)) & (m))
#define FIELD_GET(m, r)		((((unsigned long)(r)) & (m)) >> __bf_shf(m))
#define FIELD_MAX(m)		((unsigned long)(m) >> __bf_shf(m))
#endif
