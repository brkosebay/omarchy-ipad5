/* Just enough of the kernel for drivers/iio/light/tsl2583.h to build on a host. */
#ifndef SHIM_LINUX_KERNEL_H
#define SHIM_LINUX_KERNEL_H
#include <stdint.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
#define U16_MAX UINT16_MAX
#define U32_MAX UINT32_MAX
#define USEC_PER_MSEC 1000UL
#define DIV_ROUND_CLOSEST_ULL(x, d) ({ unsigned long long __x = (x); unsigned long long __d = (d); (__x + __d / 2) / __d; })
#define DIV_ROUND_CLOSEST(x, d) (((x) + (d) / 2) / (d))
#define min_t(t, a, b) ({ t __a = (a); t __b = (b); __a < __b ? __a : __b; })
#define max_t(t, a, b) ({ t __a = (a); t __b = (b); __a > __b ? __a : __b; })
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
#endif
