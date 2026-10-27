/* SPDX-License-Identifier: GPL-2.0 */
/* Host shim: kernel integer types over the installed uapi <linux/types.h>. */
#ifndef VXD_HOST_LINUX_TYPES_H
#define VXD_HOST_LINUX_TYPES_H
#include_next <linux/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t dma_addr_t;
#endif
