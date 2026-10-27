/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VXD_HOST_LINUX_SLAB_H
#define VXD_HOST_LINUX_SLAB_H
#include <stdlib.h>
#define GFP_KERNEL		0
#define kmalloc(n, gfp)		malloc(n)
#define kfree(p)		free(p)
#endif
