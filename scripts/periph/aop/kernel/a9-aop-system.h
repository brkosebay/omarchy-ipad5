/* SPDX-License-Identifier: GPL-2.0-only */
/* RTKit v12 system endpoints on the old 56-bit A7IOPV2 transport. */
#ifndef _A9_AOP_SYSTEM_H
#define _A9_AOP_SYSTEM_H

#include "a9-aop-afk.h"

#define A9_SYS_CRASHLOG 1
#define A9_SYS_SYSLOG 2
#define A9_SYS_DEBUG 3
#define A9_SYS_IOREPORT 4
#define A9_SYS_COUNT 5
#define A9_SYS_PAGE 4096U
#define A9_SYS_ADDR_MASK 0x00000fffffffffffULL

/* Firmware 010314f4/01031408: 10 <-> internal 20, 201 <-> internal 8.
 * AP QUIESCED precedes IOP resumable sleep, as in m1n1. iOS b71750 uses
 * 201 (not bare OFF=0) before stopping the A7IOPV2 CPU. Never send OFF.
 */
#define A9_SYS_AP_QUIESCED 0x10U
#define A9_SYS_IOP_SLEEP 0x201U

static inline u64 a9_sys_ap_quiesce(void)
{
	return a9_mgmt_word(0xb, A9_SYS_AP_QUIESCED);
}

static inline u64 a9_sys_iop_sleep(void)
{
	return a9_mgmt_word(6, A9_SYS_IOP_SLEEP);
}

/* These additional words require the opt-in AND the synchronous shutdown
 * owner. Do not enlarge the boot/running-attach vocabulary.
 */
static inline bool a9_sys_shutdown_tx_allowed(u8 ep, u64 payload)
{
	return !ep && (payload == a9_sys_ap_quiesce() ||
		       payload == a9_sys_iop_sleep() ||
		       (a9_mgmt_type(payload) == A9_MGMT_STARTEP &&
			(payload & 0xffff0000ffffffffULL) ==
			 a9_mgmt_word(A9_MGMT_STARTEP, 1) &&
			((payload >> 32) & 0xffff) >= 0x20 &&
			((payload >> 32) & 0xffff) <= 0xff));
}

static inline bool a9_sys_supported(unsigned int ep)
{
	return ep >= A9_SYS_CRASHLOG && ep < A9_SYS_COUNT;
}

/* RTBuddy b767f8/b6b268/b6d5f8: opcode[55:54], size unit[53:52],
 * count[51:44], PA[43:0]. Unit 1=4 KiB, 2/3=1 MiB (bit0 wins).
 * Protocol pages are independent of the AP's 16 KiB PAGE_SIZE.
 * OSLog's different 64-bit encoding cannot fit this transport; never use it.
 */
static inline int a9_sys_getbuf(u64 payload, size_t *size, u64 *pa)
{
	u8 unit = a9_mgmt_type(payload);

	if (!unit || unit > 3)
		return -EPROTO;
	*size = ((payload >> 44) & 0xff) << (unit & 1 ? 12 : 20);
	*pa = payload & A9_SYS_ADDR_MASK;
	return *size ? 0 : -EMSGSIZE;
}

static inline int a9_sys_getbuf_reply(u64 pa, size_t size, u64 *reply)
{
	unsigned int shift = size >= 0x100000 ? 20 : 12;

	if (!size || size % (1U << shift) || size >> shift > 0xff ||
	    pa % A9_SYS_PAGE || pa & ~A9_SYS_ADDR_MASK ||
	    size - 1 > A9_SYS_ADDR_MASK - pa)
		return -ERANGE;
	*reply = a9_mgmt_word(shift == 12 ? 1 : 2,
			      ((u64)(size >> shift) << 44) | pa);
	return 0;
}

/* Share one high-water mark with AFK's 64-byte allocations. Commit only on
 * success; system grants must remain 4 KiB aligned even after an AFK grant.
 */
static inline int a9_sys_alloc(size_t size, size_t capacity, size_t *used,
			       size_t *offset)
{
	size_t begin;

	if (*used > capacity || *used > (size_t)-1 - (A9_SYS_PAGE - 1) ||
	    !size || size % A9_SYS_PAGE)
		return -EINVAL;
	begin = (*used + A9_SYS_PAGE - 1) & ~(size_t)(A9_SYS_PAGE - 1);
	if (begin > capacity || size > capacity - begin)
		return -ENOSPC;
	*offset = begin;
	*used = begin + size;
	return 0;
}

/* Additional TX vocabulary only for the separately selected cold run. */
static inline bool a9_sys_tx_allowed(u8 ep, u64 payload)
{
	u8 type = a9_mgmt_type(payload);

	if (!ep)
		return payload == 0x00b0000000000020ULL;
	if (!a9_sys_supported(ep))
		return false;
	return (type == 1 || type == 2) ||
	       (ep == A9_SYS_SYSLOG && type == 5) ||
	       (ep == A9_SYS_IOREPORT && type == 8);
}

#endif
