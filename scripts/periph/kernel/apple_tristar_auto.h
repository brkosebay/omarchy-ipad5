/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J71t Lightning accessory policy: camera adapter to USB host mode.
 *
 * iOS 16.7.10 on this unit (2026-10-27 capture, DigitalID 11 f0 00 00 00 00):
 * about 1 s after the ID, IOAccessoryManagerUserClient selector 1 calls
 * configureUSBMode(2). IOAccessoryManager_updateUSBConnectType maps mode 2
 * to ConnectType 4 "Device", the connect shim publishes AppleUSBCableType
 * USBDevice, and the arbitrator applies publish mask 5: EHCI and OHCI are
 * published and the device nub stays. The adapter's hub enumerates at high
 * speed. There is no device-mode or iAP phase. The power handshake follows,
 * and the input limit is then 2100 mA at 5 V while the iPad is the host.
 *
 * This file holds only the decisions, so the offline harness runs the same
 * code as the kernel. The driver does all I/O and reports the results.
 * Times are milliseconds and may wrap.
 */
#ifndef APPLE_TRISTAR_AUTO_H
#define APPLE_TRISTAR_AUTO_H

/* CBTL1610 0x0c: both low bits are strict VBUS, bit 7 an attached plug. */
#define TRISTAR_AUTO_VBUS		0x03
#define TRISTAR_AUTO_PRESENT		0x80
#define TRISTAR_AUTO_ORIENTATION	0x40
#define TRISTAR_AUTO_ID_LEN		6

enum tristar_auto_phase {
	TRISTAR_AUTO_OFF,	/* policy disabled */
	TRISTAR_AUTO_EMPTY,	/* nothing attached */
	TRISTAR_AUTO_DEVICE,	/* attached, device role, GetID pending */
	TRISTAR_AUTO_OTHER,	/* identified, not a host-mode accessory */
	TRISTAR_AUTO_HANDSHAKE,	/* host adapter identified, 0x72/0x70 next */
	TRISTAR_AUTO_ENTER,	/* switch to host requested */
	TRISTAR_AUTO_HOST,	/* host role, owned by this policy */
	TRISTAR_AUTO_INHIBIT,	/* role taken over or entry failed; wait for unplug */
};

enum tristar_auto_action {
	TRISTAR_AUTO_NONE,
	TRISTAR_AUTO_PROBE,		/* GetID without reorient */
	TRISTAR_AUTO_POWER_HANDSHAKE,
	TRISTAR_AUTO_TO_HOST,
	TRISTAR_AUTO_TO_DEVICE,
};

struct tristar_auto_cfg {
	unsigned int probe_ms;		/* GetID period, no active USB link */
	unsigned int busy_probe_ms;	/* ... while a USB host uses the gadget */
	unsigned int verify_ms;		/* host role: re-check the ID */
	unsigned int settle_ms;		/* after a plug edge */
	unsigned int detach_polls;	/* consecutive empty 0x0c reads */
	unsigned int handshake_tries;
	int input_ua;			/* host role, after a good power handshake */
	int floor_ua;			/* host role, no power handshake */
};

struct tristar_auto {
	enum tristar_auto_phase phase;
	int status;			/* last good 0x0c, or -1 */
	unsigned int empty;		/* consecutive empty 0x0c reads */
	bool was_empty;			/* the previous read was empty */
	unsigned long next_probe;
	u8 id[TRISTAR_AUTO_ID_LEN];
	bool id_valid;
	bool adapter;			/* id selects host mode */
	int id_status;			/* last GetID result */
	int ph_status;			/* last power handshake, 1 = not run */
	unsigned int ph_tries;
	bool ph_ok;
	unsigned int probes;
	unsigned int probe_errors;
	unsigned int host_entries;
	unsigned int role_errors;
	unsigned int detaches;
	unsigned int inhibits;
};

struct tristar_auto_in {
	unsigned long now;
	int status;		/* 0x0c or -errno */
	bool enabled;
	bool host;		/* the role switch is in host mode */
	bool owned;		/* ... on this policy's request */
	bool link_active;	/* a USB host configured the gadget, not suspended */
};

static inline bool tristar_auto_due(unsigned long now, unsigned long when)
{
	return (long)(now - when) >= 0;
}

/* The camera adapter, DigitalID 11 f0 ..: USB=2 UART=1 PWR=0xf. */
static inline bool tristar_auto_is_host_adapter(const u8 *id)
{
	return id[0] == 0x11 && id[1] == 0xf0;
}

static inline bool tristar_auto_attached(int status)
{
	return status >= 0 && (status & (TRISTAR_AUTO_PRESENT | TRISTAR_AUTO_VBUS));
}

static inline const char *tristar_auto_phase_name(enum tristar_auto_phase phase)
{
	static const char *const names[] = {
		[TRISTAR_AUTO_OFF] = "off",
		[TRISTAR_AUTO_EMPTY] = "empty",
		[TRISTAR_AUTO_DEVICE] = "device",
		[TRISTAR_AUTO_OTHER] = "other",
		[TRISTAR_AUTO_HANDSHAKE] = "handshake",
		[TRISTAR_AUTO_ENTER] = "enter-host",
		[TRISTAR_AUTO_HOST] = "host",
		[TRISTAR_AUTO_INHIBIT] = "inhibit",
	};

	return (unsigned int)phase < ARRAY_SIZE(names) ? names[phase] : "?";
}

static inline void tristar_auto_forget(struct tristar_auto *ta)
{
	ta->id_valid = false;
	ta->adapter = false;
	ta->ph_ok = false;
	ta->ph_status = 1;
	ta->ph_tries = 0;
}

static inline void tristar_auto_init(struct tristar_auto *ta)
{
	*ta = (struct tristar_auto){ .phase = TRISTAR_AUTO_OFF, .status = -1, .ph_status = 1 };
}

/* One poll. The caller then runs the returned action and reports back. */
static inline enum tristar_auto_action
tristar_auto_step(struct tristar_auto *ta, const struct tristar_auto_cfg *cfg,
		  const struct tristar_auto_in *in)
{
	const int plug_bits = TRISTAR_AUTO_PRESENT | TRISTAR_AUTO_ORIENTATION;
	bool attached, edge, replug;
	int prev;

	if (!in->enabled) {
		ta->phase = TRISTAR_AUTO_OFF;
		return TRISTAR_AUTO_NONE;
	}
	if (ta->phase == TRISTAR_AUTO_OFF) {
		ta->phase = TRISTAR_AUTO_EMPTY;
		ta->empty = 0;
		ta->was_empty = false;
		ta->status = -1;
		tristar_auto_forget(ta);
	}
	/* An I2C error is no evidence either way. */
	if (in->status < 0)
		return TRISTAR_AUTO_NONE;

	attached = tristar_auto_attached(in->status);
	prev = ta->status;
	ta->status = in->status;
	replug = attached && ta->was_empty;
	ta->was_empty = !attached;
	ta->empty = attached ? 0 : ta->empty + 1;
	edge = replug || (prev >= 0 && attached && ((prev ^ in->status) & plug_bits));

	if (ta->empty >= cfg->detach_polls && ta->phase != TRISTAR_AUTO_EMPTY) {
		bool release = ta->phase == TRISTAR_AUTO_HOST && in->host && in->owned;

		ta->phase = TRISTAR_AUTO_EMPTY;
		tristar_auto_forget(ta);
		if (release) {
			ta->detaches++;
			return TRISTAR_AUTO_TO_DEVICE;
		}
		return TRISTAR_AUTO_NONE;
	}

	switch (ta->phase) {
	case TRISTAR_AUTO_EMPTY:
		if (attached) {
			ta->phase = TRISTAR_AUTO_DEVICE;
			ta->next_probe = in->now + cfg->settle_ms;
		}
		return TRISTAR_AUTO_NONE;
	case TRISTAR_AUTO_DEVICE:
	case TRISTAR_AUTO_OTHER:
		/* A host role someone else asked for is theirs to end. */
		if (!attached || in->host)
			return TRISTAR_AUTO_NONE;
		if (edge) {
			ta->phase = TRISTAR_AUTO_DEVICE;
			ta->next_probe = in->now + cfg->settle_ms;
		}
		if (!tristar_auto_due(in->now, ta->next_probe))
			return TRISTAR_AUTO_NONE;
		ta->next_probe = in->now + (in->link_active ? cfg->busy_probe_ms : cfg->probe_ms);
		ta->probes++;
		return TRISTAR_AUTO_PROBE;
	case TRISTAR_AUTO_HANDSHAKE:
		return TRISTAR_AUTO_POWER_HANDSHAKE;
	case TRISTAR_AUTO_ENTER:
		if (in->host && in->owned) {
			ta->phase = TRISTAR_AUTO_HOST;
			return TRISTAR_AUTO_NONE;
		}
		return TRISTAR_AUTO_TO_HOST;
	case TRISTAR_AUTO_HOST:
		if (!in->host || !in->owned) {
			ta->phase = TRISTAR_AUTO_INHIBIT;
			ta->inhibits++;
			return TRISTAR_AUTO_NONE;
		}
		if (!attached)
			return TRISTAR_AUTO_NONE;
		if (edge || tristar_auto_due(in->now, ta->next_probe)) {
			ta->next_probe = in->now + cfg->verify_ms;
			ta->probes++;
			return TRISTAR_AUTO_PROBE;
		}
		return TRISTAR_AUTO_NONE;
	case TRISTAR_AUTO_INHIBIT:
	case TRISTAR_AUTO_OFF:
		break;
	}
	return TRISTAR_AUTO_NONE;
}

/* GetID finished: ret 0 with a valid id, or -errno. */
static inline enum tristar_auto_action
tristar_auto_probed(struct tristar_auto *ta, const struct tristar_auto_cfg *cfg,
		    unsigned long now, int ret, const u8 *id)
{
	bool adapter;

	ta->id_status = ret;
	if (ta->phase != TRISTAR_AUTO_DEVICE && ta->phase != TRISTAR_AUTO_OTHER &&
	    ta->phase != TRISTAR_AUTO_HOST)
		return TRISTAR_AUTO_NONE;
	if (ret) {
		ta->probe_errors++;
		return TRISTAR_AUTO_NONE;
	}
	adapter = tristar_auto_is_host_adapter(id);
	memcpy(ta->id, id, TRISTAR_AUTO_ID_LEN);
	ta->id_valid = true;
	if (ta->phase == TRISTAR_AUTO_HOST) {
		if (adapter)
			return TRISTAR_AUTO_NONE;
		/* A fast swap to another accessory, e.g. the Mac cable. */
		ta->adapter = false;
		ta->ph_ok = false;
		ta->phase = TRISTAR_AUTO_OTHER;
		ta->next_probe = now + cfg->probe_ms;
		ta->detaches++;
		return TRISTAR_AUTO_TO_DEVICE;
	}
	ta->adapter = adapter;
	if (!adapter) {
		ta->phase = TRISTAR_AUTO_OTHER;
		return TRISTAR_AUTO_NONE;
	}
	ta->phase = TRISTAR_AUTO_HANDSHAKE;
	ta->ph_status = 1;
	ta->ph_tries = 0;
	ta->ph_ok = false;
	return TRISTAR_AUTO_POWER_HANDSHAKE;
}

/*
 * Power handshake finished. iOS enumerated the hub before its handshake
 * completed, so host mode does not wait for it: after the last try the port
 * becomes a host anyway, with the floor input budget.
 */
static inline enum tristar_auto_action
tristar_auto_handshake_done(struct tristar_auto *ta, const struct tristar_auto_cfg *cfg,
			    int ret)
{
	if (ta->phase != TRISTAR_AUTO_HANDSHAKE)
		return TRISTAR_AUTO_NONE;
	ta->ph_tries++;
	ta->ph_status = ret;
	ta->ph_ok = !ret;
	if (ret && ta->ph_tries < cfg->handshake_tries)
		return TRISTAR_AUTO_NONE;
	ta->phase = TRISTAR_AUTO_ENTER;
	return TRISTAR_AUTO_TO_HOST;
}

static inline void tristar_auto_role_done(struct tristar_auto *ta,
					  const struct tristar_auto_cfg *cfg,
					  unsigned long now, bool to_host, int ret)
{
	if (!to_host)
		return;
	if (ta->phase != TRISTAR_AUTO_ENTER)
		return;
	if (ret) {
		/* No retry loop: the next unplug starts over. */
		ta->role_errors++;
		ta->phase = TRISTAR_AUTO_INHIBIT;
		return;
	}
	ta->phase = TRISTAR_AUTO_HOST;
	ta->host_entries++;
	ta->next_probe = now + cfg->verify_ms;
}

/*
 * Input power while the adapter is ours. iOS: PowerHandshake Yes, USBPower
 * Yes, 2100 mA at 5 V in host mode. Returns false when the ordinary
 * device-mode rules apply.
 */
static inline bool tristar_auto_supply(const struct tristar_auto *ta,
				       const struct tristar_auto_cfg *cfg, int status,
				       int *online, int *current_ua)
{
	if (!ta->id_valid || !ta->adapter)
		return false;
	if (ta->phase != TRISTAR_AUTO_HANDSHAKE && ta->phase != TRISTAR_AUTO_ENTER &&
	    ta->phase != TRISTAR_AUTO_HOST)
		return false;
	*online = status >= 0 && (status & TRISTAR_AUTO_VBUS) == TRISTAR_AUTO_VBUS;
	*current_ua = !*online ? 0 : ta->ph_ok ? cfg->input_ua : cfg->floor_ua;
	return true;
}

#endif /* APPLE_TRISTAR_AUTO_H */
