/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _A9_AOP_H
#define _A9_AOP_H

#include <linux/types.h>

#define A9_AOP_RING_PA_MAX 4

struct a9_aop_ring_pa {
	u64 pa;
	u64 size;
};

/* Immutable apple_a9_aop.debug value, owned by the passive observer. */
bool apple_a9_aop_debug(void);

/* Independent read-only state capture excludes the active driver. */
bool apple_a9_aop_state_probe(void);

/* 1: observer; 2/3: one experimental running handshake, never cold loading. */
unsigned int apple_a9_aop_attach_stage(void);

/* Shared attach gate; the active driver additionally requires active=1. */
bool apple_a9_aop_attach(void);

/* Both ring opt-ins exclude all active-driver probes before any resources. */
bool apple_a9_aop_ring_mode(void);

bool apple_a9_aop_ring_scan_early(void);

bool apple_a9_aop_ring_scan_reserved(void);

bool apple_a9_aop_ring_pa_enabled(void);
int apple_a9_aop_ring_pa_args(struct a9_aop_ring_pa *args, unsigned int max);

#endif
