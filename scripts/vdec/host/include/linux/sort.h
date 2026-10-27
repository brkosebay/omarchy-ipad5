/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host shim: sort_r() as an insertion sort.  The kernel's is a heap sort,
 * so only inputs without equal keys (frame POCs, PicNums) sort the same.
 */
#ifndef VXD_HOST_LINUX_SORT_H
#define VXD_HOST_LINUX_SORT_H
#include <string.h>
#include <linux/types.h>
static inline void sort_r(void *base, size_t num, size_t size,
			  int (*cmp)(const void *, const void *, const void *),
			  void (*swp)(void *, void *, int), const void *priv)
{
	unsigned char *b = base, tmp[64];
	size_t i, j;

	for (i = 1; i < num; i++)
		for (j = i; j && cmp(b + (j - 1) * size, b + j * size, priv) > 0; j--) {
			memcpy(tmp, b + j * size, size);
			memcpy(b + j * size, b + (j - 1) * size, size);
			memcpy(b + (j - 1) * size, tmp, size);
		}
}
#endif
