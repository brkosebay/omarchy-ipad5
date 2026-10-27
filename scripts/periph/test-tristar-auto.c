// SPDX-License-Identifier: GPL-2.0-only
/*
 * Offline harness for drivers/power/supply/apple_tristar_auto.h (branch
 * a9/usb-host5): the Lightning accessory policy that takes the camera
 * adapter to USB host mode as iOS does (DigitalID 11 f0 -> configureUSBMode(2)
 * -> host role, power handshake, 2100 mA input in host mode). It drives the
 * exact kernel decisions with scripted 0x0c reads, GetID/handshake/role
 * results and checks every action. No device access.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -I <kernel>/drivers/power/supply \
 *      -o test-tristar-auto scripts/periph/test-tristar-auto.c
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#include "apple_tristar_auto.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		printf("FAIL %s:%d: ", __func__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

static const u8 mac_id[6] = { 0x10, 0x0c, 0, 0, 0, 0 };
static const u8 adapter_id[6] = { 0x11, 0xf0, 0, 0, 0, 0 };

/* The kernel's configuration (apple_tristar_auto_cfg). */
static const struct tristar_auto_cfg cfg = {
	.probe_ms = 2000,
	.busy_probe_ms = 10000,
	.verify_ms = 5000,
	.settle_ms = 300,
	.detach_polls = 3,
	.handshake_tries = 2,
	.input_ua = 2100000,
	.floor_ua = 500000,
};

/* A tiny world: the role switch plus scripted IC answers. */
struct world {
	struct tristar_auto ta;
	unsigned long now;
	bool enabled;
	bool host;
	bool owned;
	bool link_active;
	/* What the next GetID / handshake / role change returns */
	const u8 *next_id;
	int next_id_ret;
	int next_ph_ret;
	int next_role_ret;
	/* Counters of executed actions */
	unsigned int probes, handshakes, to_host, to_device;
};

static void world_init(struct world *w)
{
	memset(w, 0, sizeof(*w));
	tristar_auto_init(&w->ta);
	w->enabled = true;
	w->now = 100000;
}

/* Run one 250 ms poll with this 0x0c value, executing actions as the driver does. */
static enum tristar_auto_action poll(struct world *w, int status)
{
	struct tristar_auto_in in = {
		.now = w->now,
		.status = status,
		.enabled = w->enabled,
		.host = w->host,
		.owned = w->host && w->owned,
		.link_active = w->link_active,
	};
	enum tristar_auto_action first, action;
	unsigned int i;
	int ret;

	first = action = tristar_auto_step(&w->ta, &cfg, &in);
	for (i = 0; i < 4 && action != TRISTAR_AUTO_NONE; i++) {
		switch (action) {
		case TRISTAR_AUTO_PROBE:
			w->probes++;
			ret = w->next_id ? w->next_id_ret : -ETIMEDOUT;
			action = tristar_auto_probed(&w->ta, &cfg, w->now, ret,
						     w->next_id ? w->next_id : mac_id);
			break;
		case TRISTAR_AUTO_POWER_HANDSHAKE:
			w->handshakes++;
			action = tristar_auto_handshake_done(&w->ta, &cfg, w->next_ph_ret);
			break;
		case TRISTAR_AUTO_TO_HOST:
			w->to_host++;
			ret = w->next_role_ret;
			if (!ret) {
				w->host = true;
				w->owned = true;
			}
			tristar_auto_role_done(&w->ta, &cfg, w->now, true, ret);
			action = TRISTAR_AUTO_NONE;
			break;
		case TRISTAR_AUTO_TO_DEVICE:
			w->to_device++;
			if (w->owned) {
				w->host = false;
				w->owned = false;
			}
			tristar_auto_role_done(&w->ta, &cfg, w->now, false, 0);
			action = TRISTAR_AUTO_NONE;
			break;
		case TRISTAR_AUTO_NONE:
			break;
		}
	}
	w->now += 250;
	return first;
}

static void polls(struct world *w, int status, unsigned int n)
{
	while (n--)
		poll(w, status);
}

static void supply(struct world *w, int status, int *online, int *ua, bool *owned)
{
	*online = -1;
	*ua = -1;
	*owned = tristar_auto_supply(&w->ta, &cfg, status, online, ua);
}

/* Boot with the Mac cable configured: identified, never a host. */
static void test_mac_cable(void)
{
	struct world w;
	int online, ua;
	bool owned;

	world_init(&w);
	w.link_active = true;
	w.next_id = mac_id;
	polls(&w, 0xa3, 2);
	CHECK(w.probes == 0, "no GetID inside the 300 ms settle, got %u", w.probes);
	polls(&w, 0xa3, 1);	/* the GetID poll, 500 ms after the plug */
	CHECK(w.probes == 1, "first GetID after the settle, got %u", w.probes);
	CHECK(w.ta.phase == TRISTAR_AUTO_OTHER, "Mac cable phase %s",
	      tristar_auto_phase_name(w.ta.phase));
	polls(&w, 0xa3, 39);	/* up to 9.75 s after that GetID */
	CHECK(w.probes == 1, "busy link: next GetID only after 10 s, got %u", w.probes);
	polls(&w, 0xa3, 1);
	CHECK(w.probes == 2, "busy link: GetID every 10 s, got %u", w.probes);
	CHECK(w.to_host == 0 && w.handshakes == 0, "the Mac cable must not cause host/handshake");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(!owned, "the Mac cable keeps the ordinary device-mode supply rules");
}

/* Owner swaps Mac -> powered camera adapter: the iOS mode-2 path. */
static void test_swap_to_adapter(void)
{
	struct world w;
	int online, ua;
	bool owned;

	world_init(&w);
	w.link_active = true;
	w.next_id = mac_id;
	polls(&w, 0xa3, 8);
	CHECK(w.ta.phase == TRISTAR_AUTO_OTHER, "Mac identified first");
	/* Unplug for 0.5 s, i.e. two empty reads: not yet a detach. */
	w.link_active = false;
	polls(&w, 0x00, 2);
	CHECK(w.ta.phase == TRISTAR_AUTO_OTHER, "2 empty reads are not a detach");
	w.next_id = adapter_id;
	CHECK(poll(&w, 0xa3) == TRISTAR_AUTO_NONE, "the plug edge waits for the settle time");
	CHECK(w.ta.phase == TRISTAR_AUTO_DEVICE, "replug restarts identification");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(!owned, "unidentified: ordinary rules");
	CHECK(poll(&w, 0xa3) == TRISTAR_AUTO_NONE, "still settling 250 ms after the plug");
	CHECK(poll(&w, 0xa3) == TRISTAR_AUTO_PROBE, "GetID 500 ms after the plug");
	CHECK(w.handshakes == 1, "power handshake right after the adapter ID");
	CHECK(w.to_host == 1, "host role right after the handshake");
	CHECK(w.ta.phase == TRISTAR_AUTO_HOST, "phase %s", tristar_auto_phase_name(w.ta.phase));
	CHECK(w.host && w.owned, "the policy owns the host role");
	CHECK(w.ta.host_entries == 1, "one host entry");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(owned && online == 1 && ua == 2100000,
	      "host mode charges at 2100 mA: owned %d online %d ua %d", owned, online, ua);
	/* Steady host: one verify GetID every 5 s, no role change. */
	polls(&w, 0xa3, 40);
	CHECK(w.to_host == 1 && w.to_device == 0, "steady host role");
	CHECK(w.probes >= 3 && w.probes <= 4, "verify GetID every 5 s, total %u", w.probes);
	/* Unplug: exactly one device-role request after 3 empty reads. */
	polls(&w, 0x00, 2);
	CHECK(w.to_device == 0 && w.host, "no detach before 3 empty reads");
	polls(&w, 0x00, 1);
	CHECK(w.to_device == 1 && !w.host, "device role at the 3rd empty read");
	CHECK(w.ta.phase == TRISTAR_AUTO_EMPTY && !w.ta.id_valid, "state forgotten");
	polls(&w, 0x00, 20);
	CHECK(w.to_device == 1, "only one device request");
	supply(&w, 0x00, &online, &ua, &owned);
	CHECK(!owned, "nothing attached: ordinary rules");
	/* Mac back: identified, gadget restored by the role switch, no host. */
	w.next_id = mac_id;
	w.link_active = true;
	polls(&w, 0xe3, 4);
	CHECK(w.ta.phase == TRISTAR_AUTO_OTHER && w.to_host == 1, "Mac after the adapter");
}

/* Supply during the switch: never offline while VBUS is there. */
static void test_supply_through_switch(void)
{
	struct world w;
	int online, ua;
	bool owned;

	world_init(&w);
	w.next_id = adapter_id;
	w.next_ph_ret = -ETIMEDOUT;	/* first handshake fails, retried next poll */
	polls(&w, 0xa3, 3);
	CHECK(w.ta.phase == TRISTAR_AUTO_HANDSHAKE, "waiting for the handshake retry");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(owned && online == 1 && ua == 500000, "before the handshake: online at the floor");
	w.next_ph_ret = 0;
	poll(&w, 0xa3);
	CHECK(w.handshakes == 2 && w.ta.ph_ok && w.ta.phase == TRISTAR_AUTO_HOST,
	      "second try ok, host");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(owned && online == 1 && ua == 2100000, "after the handshake: 2100 mA");
	/* Adapter's own supply unplugged: still attached, no input. */
	poll(&w, 0x80);
	supply(&w, 0x80, &online, &ua, &owned);
	CHECK(owned && online == 0 && ua == 0, "no VBUS: offline, %d %d", online, ua);
	CHECK(w.host, "the adapter is still there: still a host");
}

/* iOS enumerated the hub before its handshake: host even if it fails. */
static void test_handshake_fails(void)
{
	struct world w;
	int online, ua;
	bool owned;

	world_init(&w);
	w.next_id = adapter_id;
	w.next_ph_ret = -EPROTO;
	polls(&w, 0xa3, 6);
	CHECK(w.handshakes == 2, "two tries, got %u", w.handshakes);
	CHECK(w.ta.phase == TRISTAR_AUTO_HOST && w.host, "host anyway");
	supply(&w, 0xa3, &online, &ua, &owned);
	CHECK(owned && online == 1 && ua == 500000, "no handshake: floor budget");
}

/* The user takes the role back: no fight until the adapter is unplugged. */
static void test_user_override(void)
{
	struct world w;

	world_init(&w);
	w.next_id = adapter_id;
	polls(&w, 0xa3, 3);
	CHECK(w.ta.phase == TRISTAR_AUTO_HOST, "host");
	w.host = false;		/* linux-a9-usb-host device */
	w.owned = false;
	polls(&w, 0xa3, 60);
	CHECK(w.ta.phase == TRISTAR_AUTO_INHIBIT, "inhibited, phase %s",
	      tristar_auto_phase_name(w.ta.phase));
	CHECK(w.to_host == 1 && w.to_device == 0, "no new host entry, no device request");
	polls(&w, 0x00, 3);
	CHECK(w.ta.phase == TRISTAR_AUTO_EMPTY, "unplug clears the inhibit");
	polls(&w, 0xa3, 3);
	CHECK(w.to_host == 2 && w.host, "replug: host again");
}

/* A manual host role (linux-a9-usb-host arm) is not the policy's to end. */
static void test_manual_host(void)
{
	struct world w;

	world_init(&w);
	w.next_id = adapter_id;
	w.host = true;		/* manual arm, not owned */
	polls(&w, 0xa3, 20);
	CHECK(w.probes == 0 && w.to_host == 0, "no GetID while someone else holds host");
	polls(&w, 0x00, 5);
	CHECK(w.to_device == 0 && w.host, "unplug does not end a manual host role");
}

/* I2C errors are no evidence of an unplug. */
static void test_i2c_errors(void)
{
	struct world w;

	world_init(&w);
	w.next_id = adapter_id;
	polls(&w, 0xa3, 3);
	CHECK(w.host, "host");
	polls(&w, -EIO, 10);
	CHECK(w.host && w.to_device == 0, "errors kept the host role");
	CHECK(w.ta.empty == 0, "errors do not count as empty reads");
}

/* A fast swap adapter -> Mac with no empty read: the verify GetID catches it. */
static void test_fast_swap(void)
{
	struct world w;

	world_init(&w);
	w.next_id = adapter_id;
	polls(&w, 0xa3, 3);
	CHECK(w.host, "host");
	w.next_id = mac_id;
	polls(&w, 0xa3, 21);	/* > verify_ms */
	CHECK(!w.host && w.to_device == 1, "Mac ID in host mode -> device role");
	CHECK(w.ta.phase == TRISTAR_AUTO_OTHER, "phase %s", tristar_auto_phase_name(w.ta.phase));
	polls(&w, 0xa3, 40);
	CHECK(w.to_host == 1, "no host for the Mac");
}

/* One empty read in host mode is a replug: verify at once, no detach. */
static void test_glitch(void)
{
	struct world w;
	unsigned int probes;

	world_init(&w);
	w.next_id = adapter_id;
	polls(&w, 0xa3, 3);
	probes = w.probes;
	poll(&w, 0x00);
	CHECK(poll(&w, 0xe3) == TRISTAR_AUTO_PROBE, "replug edge -> verify GetID");
	CHECK(w.probes == probes + 1 && w.host && w.to_device == 0, "still a host");
	/* bit 7 clear with VBUS (0x2b, seen once on hardware) is attached */
	polls(&w, 0x2b, 5);
	CHECK(w.host && w.to_device == 0, "0x2b is not empty");
}

/* Probing cadence without a USB link, and GetID failures. */
static void test_probe_cadence(void)
{
	struct world w;

	world_init(&w);
	w.next_id = NULL;	/* every GetID times out (e.g. a dumb charger) */
	polls(&w, 0xa3, 3);
	CHECK(w.probes == 1, "first GetID at the first poll after 300 ms");
	polls(&w, 0xa3, 7);
	CHECK(w.probes == 1, "next only after 2 s");
	polls(&w, 0xa3, 1);
	CHECK(w.probes == 2, "every 2 s with no USB link, got %u", w.probes);
	CHECK(w.ta.probe_errors == 2 && w.ta.phase == TRISTAR_AUTO_DEVICE, "errors counted");
	CHECK(w.to_host == 0, "no host without an ID");
	polls(&w, 0x00, 3);
	polls(&w, 0x00, 40);
	CHECK(w.probes == 2, "no GetID with nothing attached");
}

/* A failed host entry does not loop. */
static void test_role_failure(void)
{
	struct world w;

	world_init(&w);
	w.next_id = adapter_id;
	w.next_role_ret = -ENODEV;
	polls(&w, 0xa3, 40);
	CHECK(w.to_host == 1, "one attempt, got %u", w.to_host);
	CHECK(w.ta.phase == TRISTAR_AUTO_INHIBIT && w.ta.role_errors == 1, "inhibited");
	w.next_role_ret = 0;
	polls(&w, 0x00, 3);
	polls(&w, 0xa3, 3);
	CHECK(w.to_host == 2 && w.host, "retried after a replug");
}

/* auto_host=0: nothing happens; re-enabled: starts over. */
static void test_disable(void)
{
	struct world w;

	world_init(&w);
	w.enabled = false;
	w.next_id = adapter_id;
	polls(&w, 0xa3, 20);
	CHECK(w.probes == 0 && w.ta.phase == TRISTAR_AUTO_OFF, "disabled");
	w.enabled = true;
	polls(&w, 0xa3, 3);
	CHECK(w.host && w.to_host == 1, "enabled: host");
}

/* The ID match itself. */
static void test_id_match(void)
{
	static const u8 other[][6] = {
		{ 0x10, 0x0c, 0, 0, 0, 0 },	/* Mac cable */
		{ 0x11, 0x00, 0, 0, 0, 0 },
		{ 0x01, 0xf0, 0, 0, 0, 0 },
		{ 0x00, 0x00, 0, 0, 0, 0 },
	};
	unsigned int i;

	CHECK(tristar_auto_is_host_adapter(adapter_id), "11 f0 00 00 00 00 is the adapter");
	for (i = 0; i < ARRAY_SIZE(other); i++)
		CHECK(!tristar_auto_is_host_adapter(other[i]), "%02x %02x is not", other[i][0],
		      other[i][1]);
	CHECK(!strcmp(tristar_auto_phase_name(TRISTAR_AUTO_HOST), "host"), "names");
	CHECK(!strcmp(tristar_auto_phase_name((enum tristar_auto_phase)99), "?"), "bad name");
}

/* Time wraps safely. */
static void test_wrap(void)
{
	struct world w;

	world_init(&w);
	w.now = (unsigned long)-600;
	w.next_id = adapter_id;
	polls(&w, 0xa3, 4);
	CHECK(w.host, "host across the wrap");
}

int main(void)
{
	test_mac_cable();
	test_swap_to_adapter();
	test_supply_through_switch();
	test_handshake_fails();
	test_user_override();
	test_manual_host();
	test_i2c_errors();
	test_fast_swap();
	test_glitch();
	test_probe_cadence();
	test_role_failure();
	test_disable();
	test_id_match();
	test_wrap();
	if (failures) {
		printf("FAIL: %d of %d checks\n", failures, checks);
		return 1;
	}
	printf("PASS tristar accessory policy: %d checks\n", checks);
	return 0;
}
