/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _A9_AOP_EXPERIMENT_H
#define _A9_AOP_EXPERIMENT_H

#include "a9-aop-afk.h"

/* Board-specific, permanently excluded from Linux allocation in tri49a. */
#define A9_EXP_POOL_PA 0x858000000ULL
#define A9_EXP_POOL_SIZE 0x400000U
#define A9_EXP_SAMPLES 128
#define A9_EXP_REG_TRACE 640

enum a9_exp_action { A9_EXP_WAIT, A9_EXP_SEND, A9_EXP_UNMASK, A9_EXP_WAKE };

enum a9_exp_wake {
	A9_EXP_WAKE_NONE,
	A9_EXP_WAKE_IOP,
	A9_EXP_WAKE_INBOX,
	A9_EXP_WAKE_RUN,
	A9_EXP_WAKE_AUTOBOOT,
	A9_EXP_WAKE_COMBINED,
};

/* The final three steps require the separate cycle=1 boot opt-in. */
#define A9_EXP_CYCLE_STEPS 3
static const struct a9_exp_step {
	const char *name;
	enum a9_exp_action action;
	u64 word;
	unsigned int ms;
} a9_exp_steps[] = {
	{ "passive", A9_EXP_WAIT, 0, 5000 },
	{ "hello12", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
	{ "mailbox-unmask", A9_EXP_UNMASK, 0, 1000 },
	{ "ping0", A9_EXP_SEND, 0x0030000000000000ULL, 500 },
	{ "wake220", A9_EXP_SEND, 0x0060000000000220ULL, 2000 },
	{ "hello-after-wake", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
	{ "ap-awake-b20", A9_EXP_SEND, 0x00b0000000000020ULL, 1000 },
	{ "hello-after-ap", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
	{ "quiesce201", A9_EXP_SEND, 0x0060000000000201ULL, 500 },
	{ "rewake220", A9_EXP_SEND, 0x0060000000000220ULL, 2000 },
	{ "hello-after-cycle", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
};

/* Separate boots isolate hardware wake; no sleep cycle or old AP-mask ladder. */
static const struct a9_exp_step a9_exp_wake_steps[] = {
	{ "passive", A9_EXP_WAIT, 0, 5000 },
	{ "hardware-wake", A9_EXP_WAKE, 0, 1000 },
	{ "ping0", A9_EXP_SEND, 0x0030000000000000ULL, 500 },
	{ "wake220", A9_EXP_SEND, 0x0060000000000220ULL, 2000 },
	{ "hello12", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
	{ "ap-awake-b20", A9_EXP_SEND, 0x00b0000000000020ULL, 1000 },
	{ "hello-after-ap", A9_EXP_SEND, 0x00200000000c000cULL, 2000 },
};

static inline int a9_exp_alloc(size_t size, size_t capacity, size_t *used,
			       size_t *offset)
{
	size_t rounded;

	if (!size || size > capacity || *used > capacity ||
	    size > (size_t)-1 - 63)
		return -EINVAL;
	rounded = (size + 63) & ~(size_t)63;
	if (rounded > capacity - *used)
		return -ENOSPC;
	*offset = *used;
	*used += rounded;
	return 0;
}

/* Only monotonically increasing chunks covering the 8-bit endpoint space. */
static inline int a9_exp_epmap(unsigned int base, unsigned int *seen)
{
	if (base >= 8 || (*seen && (1U << base) <= *seen))
		return -EPROTO;
	*seen |= 1U << base;
	return 0;
}

static inline bool a9_exp_tx_allowed(unsigned int stage, u8 ep, u64 payload)
{
	u8 type;

	if (!ep) {
		type = a9_mgmt_type(payload);
		if (type == A9_MGMT_HELLO_REPLY || type == A9_MGMT_EPMAP ||
		    type == A9_MGMT_AP_POWER)
			return true;
		if (type == 6)
			return payload == 0x0060000000000220ULL ||
			       payload == 0x0060000000000201ULL;
		if (type == A9_MGMT_AP_POWER_ACK)
			return payload == 0x00b0000000000020ULL;
		if (type == A9_MGMT_PING && !(u32)payload)
			return true;
		return stage == 3 && (type == A9_MGMT_PING ||
				      type == A9_MGMT_STARTEP);
	}
	if (stage != 3 || a9_aop_system_endpoint(ep))
		return false;
	type = a9_afk_type(payload);
	return type == A9_AFK_INIT_ACK || type == A9_AFK_GETBUF_ACK ||
	       type == A9_AFK_DOORBELL || type == A9_AFK_START ||
	       type == A9_AFK_SHUTDOWN;
}

#endif
