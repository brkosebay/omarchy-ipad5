/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_TRISTAR_SOURCE_H
#define APPLE_TRISTAR_SOURCE_H

struct power_supply;

/* A physical source epoch, independent of USB suspend/reset notifications. */
int apple_tristar_attach_id(struct power_supply *psy, unsigned int *id);

#endif
