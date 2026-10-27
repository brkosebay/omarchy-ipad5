/* SPDX-License-Identifier: GPL-2.0-only */
/* Production claim against the arm64 no-map + FDT reservation hierarchy. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IORESOURCE_MEM 0x200
#define IORESOURCE_BUSY 0x80000000U
#define IORESOURCE_SYSRAM 0x1000000
#define EBUSY 16
#define dev_err(...) ((void)0)
typedef uint64_t resource_size_t;
struct resource {
	const char *name;
	resource_size_t start, end;
	unsigned int flags;
	struct resource *child, *sibling;
};
struct a9_aop { int dev; struct resource pool_resource; };
static struct resource iomem_resource = { .end = UINT64_MAX };
static resource_size_t resource_size(struct resource *r) { return r->end - r->start + 1; }
static unsigned int resource_type(struct resource *r) { return r->flags & 0xf00; }
static struct resource *lookup_resource(struct resource *r, resource_size_t start)
{
	for (r = r->child; r; r = r->sibling) if (r->start == start) return r;
	return NULL;
}
static int devm_request_resource(int dev, struct resource *parent, struct resource *new)
{
	(void)dev;
	for (struct resource *r = parent->child; r; r = r->sibling)
		if (r->start <= new->end && r->end >= new->start) return -EBUSY;
	parent->child = new;
	return 0;
}
#include "pool-functions.h"
int main(void)
{
	const resource_size_t base = 0x858000000ULL, size = 0x400000;
	struct a9_aop a = {0}, b = {0};
	struct resource outer = { .name = "reserved", .start = base, .end = base + size - 1,
		.flags = IORESOURCE_MEM };
	struct resource busy = outer;
	busy.flags |= IORESOURCE_BUSY; outer.child = &busy; iomem_resource.child = &outer;
	assert(!a9_aop_pool_claim(&a, base, size));
	assert(busy.child == &a.pool_resource && !strcmp(a.pool_resource.name, "a9-aop-stage3-pool"));
	assert(a9_aop_pool_claim(&b, base, size) == -EBUSY); /* Exclusive second claim. */
	busy.child = NULL; busy.name = "other-driver";
	assert(a9_aop_pool_claim(&a, base, size) == -EBUSY);
	busy.name = "reserved"; busy.flags |= IORESOURCE_SYSRAM;
	assert(a9_aop_pool_claim(&a, base, size) == -EBUSY);
	busy.flags &= ~IORESOURCE_SYSRAM; busy.end--;
	assert(a9_aop_pool_claim(&a, base, size) == -EBUSY);
	busy.start++; /* Partial overlap with a different start fails the actual claim. */
	assert(a9_aop_pool_claim(&a, base, size) == -EBUSY);
	outer.child = NULL;
	assert(!a9_aop_pool_claim(&a, base, size)); /* no duplicate header node */
	iomem_resource.child = NULL;
	assert(a9_aop_pool_claim(&b, base, size) == -EBUSY);
	puts("PASS production pool claim: duplicate generic reservations, exclusive child, driver/System RAM/partial-overlap refusal");
	return 0;
}
