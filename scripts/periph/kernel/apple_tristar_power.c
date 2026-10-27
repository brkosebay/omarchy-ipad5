// SPDX-License-Identifier: GPL-2.0-only
/* J71t CBTL1610 source detection, reconstructed from the iOS kernelcache. */
#include <linux/array_size.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb/apple-s8000-role.h>
#include <linux/usb/gadget.h>
#include <linux/usb/role.h>

#include "apple_tristar_idbus.h"
#include "apple_tristar_auto.h"
#include "apple_tristar_source.h"

#define TRISTAR_VBUS 0x0c
#define TRISTAR_REVISION 0x0f
#define TRISTAR_J71T_REVISION 0x8a
#define TRISTAR_DX_CONTROL 0x01
#define TRISTAR_DCP_CONTROL 0x03
#define D2365_ADC_DONE 0x156
#define D2365_ADC_START 0x4000
#define D2365_ADC_RESULT 0x4001

static bool allow_power_handshake;
module_param(allow_power_handshake, bool, 0644);
MODULE_PARM_DESC(allow_power_handshake,
		 "Permit the debugfs-triggered 0x72/0x70 ID-bus power handshake (default: off)");

static bool auto_host = true;
module_param(auto_host, bool, 0644);
MODULE_PARM_DESC(auto_host,
		 "Identify Lightning accessories and make the port a USB host for the camera adapter, as iOS does (default: on)");

#define APPLE_TRISTAR_FLOOR_UA		500000
#define APPLE_TRISTAR_ADAPTER_MAX_UA	2100000

static unsigned int auto_input_ua = APPLE_TRISTAR_ADAPTER_MAX_UA;
module_param(auto_input_ua, uint, 0644);
MODULE_PARM_DESC(auto_input_ua,
		 "Input current offered by the camera adapter after its power handshake (default and maximum: 2100000, iOS's limit)");

/* Last manual ID-bus runs, published read-only in debugfs. */
struct apple_tristar_idbus {
	struct tristar_idbus ib;
	struct tristar_idbus_id_result id;
	struct tristar_idbus_ph_result ph;
	struct tristar_idbus_recover_result rc;
	unsigned long id_at;
	bool id_run;
	bool ph_run;
	bool rc_run;
	/* Automatic GetID: transcript only, no kernel log lines */
	bool quiet;
	char *log;
	size_t log_len;
	size_t log_size;
	char id_log[4096];
	char ph_log[2048];
	char rc_log[2048];
};

struct apple_tristar {
	struct i2c_client *client;
	struct power_supply *psy;
	struct regmap *pmic;
	struct device_node *usb_node;
	struct usb_role_switch *role_sw;
	struct apple_tristar_idbus *idbus;
	struct dentry *debugfs;
	struct notifier_block usb_notifier;
	atomic_t reset_generation;
	int seen_generation;
	atomic_t usb_session;
	atomic_t computer_seen;
	atomic_t usb_events;
	atomic_t usb_idle_events;
	unsigned long usb_event_source;
	int bc_ua;
	int bc_type;
	int cc_first_mv;
	int bc_primary_mv;
	int bc_secondary_mv;
	u8 source_id[TRISTAR_AUTO_ID_LEN];
	bool source_id_valid;
	bool epoch_notify;
	const char *epoch_reason;
	struct delayed_work work;
	/* Serializes register reads and protects the cached source state. */
	struct mutex lock;
	/* Lightning accessory policy, its last 0x0c read and its log */
	struct tristar_auto autop;
	int status;
	char auto_log[2048];
	size_t auto_log_len;
	int online;
	int error;
	int current_ua;
	int usb_type;
	int brick_ua;
	int analog_type;
	unsigned int attach_id;
	bool source_present;
	bool computer;
	bool detach_role;
	unsigned long classify_after;
	unsigned long classify_log_after;
	const char *detect_stage;
	const char *detect_pin;
	int detect_error;
	int monitor_error;
	int sample_status;
	int route_before;
	int adc_status;
	int dp_mv;
	int dm_mv;
	int cc_mv;
	unsigned int detect_attempts;
	unsigned int detect_failures;
	unsigned int retry_ms;
	bool detect_logged;
	u8 saved_route;
	bool restore_pending;
	bool dcp_pending;
	bool brick_sampled;
	bool adc_poisoned;
	const char *adc_fault_stage;
	unsigned int adc_fault_attach;
	bool stopping;
	bool notifier_registered;
	bool sleeping;
};

static int apple_tristar_read(struct i2c_client *client, u8 reg)
{
	union i2c_smbus_data data;
	int ret;

	/* iOS op=1: selector + STOP, then a separate read START. */
	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);
	ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			       I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE, NULL);
	if (!ret)
		ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				       I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	return ret ?: data.byte;
}

static int apple_tristar_write_reg_checked(struct apple_tristar *ts, u8 reg, u8 value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(ts->client, reg, value);
	if (ret)
		return ret;
	ret = apple_tristar_read(ts->client, reg);
	return ret < 0 ? ret : ret == value ? 0 : -EIO;
}

static int apple_tristar_write_checked(struct apple_tristar *ts, u8 value)
{
	return apple_tristar_write_reg_checked(ts, TRISTAR_DX_CONTROL, value);
}

static int apple_tristar_restore_route(struct apple_tristar *ts)
{
	int ret;

	if (ts->dcp_pending) {
		ret = apple_tristar_write_reg_checked(ts, TRISTAR_DCP_CONTROL, 0);
		if (ret)
			return ret;
		ts->dcp_pending = false;
	}
	if (!ts->restore_pending)
		return 0;
	ret = apple_tristar_write_checked(ts, ts->saved_route);
	if (!ret)
		ts->restore_pending = false;
	return ret;
}

static int apple_tristar_pmic_read(struct apple_tristar *ts, unsigned int reg)
{
	unsigned int value;
	int ret = regmap_read(ts->pmic, reg, &value);

	return ret ?: value;
}

/* Exclusive ADC use. Never re-trigger after uncertain completion. */
static int apple_tristar_adc_mv(struct apple_tristar *ts)
{
	unsigned int raw, q16;
	u8 result[2];
	int status, ret;

	if (ts->adc_poisoned)
		return -EIO;
	/* Observe the completion latch LOW before starting a new sample. */
	ts->detect_stage = "adc-idle";
	ret = read_poll_timeout(apple_tristar_pmic_read, status,
				status < 0 || !(status & 1), 1000, 20000,
				false, ts, D2365_ADC_DONE);
	ts->adc_status = status;
	if (ret || status < 0)
		return ret ?: status;
	ts->detect_stage = "adc-trigger";
	ret = regmap_write(ts->pmic, D2365_ADC_START, 1);
	if (ret)
		goto poison;
	ts->detect_stage = "adc-done";
	ret = read_poll_timeout(apple_tristar_pmic_read, status,
				status < 0 || (status & 1), 1000, 1000000,
				false, ts, D2365_ADC_DONE);
	ts->adc_status = status;
	if (ret || status < 0) {
		ret = ret ?: status;
		goto poison;
	}
	ts->detect_stage = "adc-result";
	ret = regmap_bulk_read(ts->pmic, D2365_ADC_RESULT, result, sizeof(result));
	if (ret)
		return ret;
	if (result[0] & 0x80)
		return -ENODATA;
	raw = (result[0] & 0x0f) | (result[1] << 4);
	/* Exact positive Q16-volts conversion, then the source's integer mV. */
	q16 = div_u64((40ULL << 16) + raw * 5000ULL * 16, 1000);
	return (q16 * 1000U) >> 16;
poison:
	/* A late completion must never be mistaken for the next pin's sample. */
	ts->adc_poisoned = true;
	ts->adc_fault_stage = ts->detect_stage;
	ts->adc_fault_attach = ts->attach_id;
	return ret;
}

/* Caller holds the role hardware lock and ts->lock. */
static bool apple_tristar_sample_valid(struct apple_tristar *ts)
{
	struct usb_gadget_current_budget budget;
	int ret = usb_gadget_get_current_budget(ts->usb_node, &budget);

	if (ret) {
		ts->detect_stage = "usb-budget";
		return false;
	}
	if (budget.connected || budget.configured || budget.host_grant ||
	    atomic_read(&ts->computer_seen)) {
		ts->detect_stage = "usb-active";
		return false;
	}
	if (ts->seen_generation != atomic_read(&ts->reset_generation)) {
		ts->detect_stage = "usb-event";
		return false;
	}
	return true;
}

static int apple_tristar_sample_vbus(struct apple_tristar *ts)
{
	int status;

	if (!apple_tristar_sample_valid(ts))
		return -EAGAIN;
	ts->detect_stage = "vbus";
	status = apple_tristar_read(ts->client, TRISTAR_VBUS);
	if (status < 0)
		return status;
	if ((status & 3) != 3)
		return -ENOLINK;
	ts->detect_stage = "orientation";
	if (!(status & TRISTAR_AUTO_PRESENT) ||
	    ((status ^ ts->sample_status) & TRISTAR_AUTO_ORIENTATION))
		return -EAGAIN;
	return apple_tristar_sample_valid(ts) ? 0 : -EAGAIN;
}

/*
 * Normal USB: monitor the oriented pair. Type-C CC: D+ on the OTHER pair
 * (iOS 5e23a90 -> 5fe52dc -> 5ff5ed0), never a fixed cable-ID allowance.
 * A zero mux is valid after replug: the direct iOS lane monitor also accepts
 * it. Only touch an idle or USB-selected nibble; restore the exact byte.
 */
static int apple_tristar_pin_mv(struct apple_tristar *ts, bool dm, bool dcp, bool cc)
{
	int ret, restore, nibble;
	bool high = !!(ts->sample_status & TRISTAR_AUTO_ORIENTATION) ^ cc;
	u8 route;

	ts->detect_pin = cc ? "cc" : dcp ? dm ? "primary" : "secondary" : dm ? "dm" : "dp";
	ret = apple_tristar_sample_vbus(ts);
	if (ret)
		return ret;
	ts->detect_stage = "route-read";
	ret = apple_tristar_read(ts->client, TRISTAR_DX_CONTROL);
	ts->route_before = ret;
	if (ret < 0)
		return ret;
	ts->saved_route = ret;
	nibble = high ? ret >> 4 : ret & 15;
	ts->detect_stage = "route-select";
	if (nibble != 0 && (nibble & 7) != 1)
		return -EOPNOTSUPP;
	route = high ? (ret & 0x0f) | (dm ? 0xd0 : 0xc0) :
		       (ret & 0xf0) | (dm ? 0x0d : 0x0c);
	if (!apple_tristar_sample_valid(ts))
		return -EAGAIN;
	ts->restore_pending = true;
	ts->detect_stage = "route-write";
	ret = apple_tristar_write_checked(ts, route);
	if (!ret) {
		usleep_range(5000, 6000);
		if (dcp) {
			/* iOS 5ff5a08: monitor 4 (D-), 5 (D+), 40 ms pulse. */
			u8 pulse = (dm ? 9 : 10) + (high ? 2 : 0);

			ts->dcp_pending = true;
			ts->detect_stage = "dcp-write";
			ret = apple_tristar_write_reg_checked(ts, TRISTAR_DCP_CONTROL, pulse);
			if (!ret)
				msleep(40);
		}
		if (!ret)
			ret = apple_tristar_sample_valid(ts) ? apple_tristar_adc_mv(ts) : -EAGAIN;
	}
	restore = apple_tristar_restore_route(ts);
	if (restore) {
		ts->detect_stage = "restore";
		return restore;
	}
	/* Preserve the original error/stage unless the conversion succeeded. */
	if (ret < 0)
		return ret;
	restore = apple_tristar_sample_vbus(ts);
	return restore ?: ret;
}

/* Verified 10 09 08 cable: USB=2, CC=1 (5e34408/5e4d9ac). */
static bool apple_tristar_typec_cable(struct apple_tristar *ts)
{
	static const u8 id[] = { 0x10, 0x09, 0x08, 0, 0, 0 };

	return ts->autop.id_valid && !memcmp(ts->autop.id, id, sizeof(id));
}

/* IOAccessoryPowerSourceItemUSB_TypeC_Current, 5de4024, inclusive edges. */
static int apple_tristar_cc_current(int mv)
{
	if (mv < 0 || mv > 4000)
		return -ERANGE;
	return mv > 1229 ? 3000000 : mv > 659 ? 1500000 : 0;
}

static int apple_tristar_brick_current(int dp_mv, int dm_mv)
{
	static const int limits[] = { 500, 1000, 1000, 2100, 2400, 2400, 1000, 1000, 1000 };
	int dp, dm;

	if (dp_mv < 0 || dm_mv < 0 || dp_mv > 4000 || dm_mv > 4000)
		return -ERANGE;
	if (dp_mv < 1500 || dm_mv < 1500)
		return 0;
	dp = dp_mv >= 2880 ? 2 : dp_mv >= 2200 ? 1 : 0;
	dm = dm_mv >= 2880 ? 2 : dm_mv >= 2200 ? 1 : 0;
	return limits[3 * dp + dm] * 1000;
}

static void apple_tristar_detect_failed(struct apple_tristar *ts, int ret)
{
	ts->detect_error = ret;
	ts->detect_failures++;
	ts->retry_ms = min(1000U << min(ts->detect_failures - 1, 6U), 60000U);
	ts->classify_after = jiffies + msecs_to_jiffies(ts->retry_ms);
	/* No repeated 1 Hz warnings, even if the global printk bucket refills. */
	if (!ts->detect_logged || !time_before(jiffies, ts->classify_log_after)) {
		dev_warn_ratelimited(&ts->client->dev,
				     "charger detect: %s/%s error %d route %d adc %d poisoned %d attempt %u retry %u ms\n",
				     ts->detect_pin ?: "none", ts->detect_stage ?: "none", ret,
				     ts->route_before, ts->adc_status, ts->adc_poisoned,
				     ts->detect_attempts, ts->adc_poisoned ? 0 : ts->retry_ms);
		ts->classify_log_after = jiffies + msecs_to_jiffies(60000);
		ts->detect_logged = true;
	}
}

static void apple_tristar_classify(struct apple_tristar *ts)
{
	int dp, dm, ret, cc, attempt, cc_ua = 0, type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int bc_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	bool typec = apple_tristar_typec_cable(ts);

	/* Caller holds the role hardware lock before ts->lock. */
	if (ts->computer || atomic_read(&ts->computer_seen) ||
	    (ts->brick_sampled && !typec) || ts->adc_poisoned ||
	    !ts->online || ts->error || time_before(jiffies, ts->classify_after))
		return;
	ts->seen_generation = atomic_read(&ts->reset_generation);
	if (!apple_tristar_sample_valid(ts))
		return;
	ts->detect_attempts++;
	ts->sample_status = ts->status;
	ts->dp_mv = -ENODATA;
	ts->dm_mv = -ENODATA;
	ts->cc_mv = -ENODATA;
	if (typec) {
		cc_ua = -1;
		/* Ignore one unsettled first reading; require two equal advertisements. */
		for (attempt = 0; attempt < 3; attempt++) {
			cc = apple_tristar_pin_mv(ts, false, false, true);
			ts->cc_mv = cc;
			if (!attempt)
				ts->cc_first_mv = cc;
			if (cc < 0) {
				ret = cc;
				goto fail;
			}
			ts->detect_stage = "cc-range";
			ret = apple_tristar_cc_current(cc);
			if (ret < 0)
				goto fail;
			if (ret == cc_ua)
				break;
			cc_ua = ret;
			if (attempt < 2)
				msleep(20);
		}
		ts->detect_stage = "cc-debounce";
		if (attempt == 3) {
			ret = -EAGAIN;
			goto fail;
		}
		if (ts->bc_ua) {
			/* CC refresh cannot erase a proven BC item from this epoch. */
			ret = ts->bc_ua;
			type = ts->bc_type;
			goto combine;
		}
	}
	dp = apple_tristar_pin_mv(ts, false, false, false);
	ts->dp_mv = dp;
	if (dp < 0) {
		ret = dp;
		goto fail;
	}
	dm = apple_tristar_pin_mv(ts, true, false, false);
	ts->dm_mv = dm;
	if (dm < 0) {
		ret = dm;
		goto fail;
	}
	ts->detect_stage = "divider-range";
	ret = apple_tristar_brick_current(dp, dm);
	if (ret < 0)
		goto fail;
	if (ret) {
		type = POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID;
	} else {
		/* iOS 5de33e4/5de35dc, ordinary USB source (connect type 0). */
		dm = apple_tristar_pin_mv(ts, true, true, false);
		ts->bc_primary_mv = dm;
		if (dm < 0) {
			ret = dm;
			goto fail;
		}
		ts->detect_stage = "primary-range";
		if (dm > 4000) {
			ret = -ERANGE;
			goto fail;
		}
		if (dm >= 326 && dm <= 899) {
			dp = apple_tristar_pin_mv(ts, false, true, false);
			ts->bc_secondary_mv = dp;
			if (dp < 0) {
				ret = dp;
				goto fail;
			}
			ts->detect_stage = "secondary-range";
			if (dp > 4000) {
				ret = -ERANGE;
				goto fail;
			}
			/* iOS internal 2=CDP, 1=DCP (type names at 6de2df0). */
			type = dp < 326 ? POWER_SUPPLY_USB_TYPE_CDP : POWER_SUPPLY_USB_TYPE_DCP;
			ret = 1500000;
		}
	}
combine:
	bc_type = type;
	/* iOS 5e23a90: without a charging-port type, clamp 3 A CC to 1.5 A. */
	if (cc_ua == 3000000 && type != POWER_SUPPLY_USB_TYPE_DCP &&
	    type != POWER_SUPPLY_USB_TYPE_CDP)
		cc_ua = 1500000;
	if (cc_ua && cc_ua >= ret) {
		ret = cc_ua;
		type = POWER_SUPPLY_USB_TYPE_C;
	}
	cc = apple_tristar_sample_vbus(ts);
	if (cc) {
		ret = cc;
		goto fail;
	}
	if (bc_type == POWER_SUPPLY_USB_TYPE_DCP || bc_type == POWER_SUPPLY_USB_TYPE_CDP) {
		ts->bc_ua = 1500000;
		ts->bc_type = bc_type;
	}
	/* Refresh Type-C advertisement, including reductions, while idle. */
	ts->classify_after = jiffies + msecs_to_jiffies(5000);
	if (!ts->brick_sampled || ts->brick_ua != ret || ts->analog_type != type)
		dev_info_ratelimited(&ts->client->dev,
				     "charger detected type %d budget %d mA dp %d dm %d cc %d mV\n",
				     type, ret / 1000, ts->dp_mv, ts->dm_mv, ts->cc_mv);
	ts->brick_ua = ret;
	ts->analog_type = type;
	ts->brick_sampled = true;
	ts->detect_error = 0;
	ts->detect_stage = "complete";
	ts->detect_failures = 0;
	ts->retry_ms = 0;
	return;
fail:
	/* Revoke uncertain CC, retaining only separately verified BC evidence. */
	if (typec) {
		ts->brick_ua = ts->bc_ua;
		ts->analog_type = ts->bc_type;
		ts->brick_sampled = false;
	}
	apple_tristar_detect_failed(ts, ret);
}

/* Never call USB role APIs while holding ts->lock: role changes unbind UDC. */
static int apple_tristar_monitor(void *data)
{
	struct apple_tristar *ts = data;
	int ret;

	mutex_lock(&ts->lock);
	/* Repair even when quiescing; never restore outside the role lock. */
	ret = apple_tristar_restore_route(ts);
	if (ret) {
		ts->detect_stage = "restore";
		ts->detect_error = ret;
	} else if (!ts->stopping && !ts->sleeping) {
		apple_tristar_classify(ts);
	}
	if (ts->restore_pending || ts->dcp_pending)
		ret = -EUCLEAN;
	mutex_unlock(&ts->lock);
	return ret;
}

static int apple_tristar_budget(const struct usb_gadget_current_budget *budget,
				int brick_ua, int *type)
{
	*type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	if (!budget)
		return 100000;
	if (budget->connected) {
		*type = POWER_SUPPLY_USB_TYPE_SDP;
		if (budget->suspended)
			return 500000; /* Link PM does not remove the enumerated source. */
		if (budget->host_grant)
			return min(budget->current_ma, 2400U) * 1000;
		if (budget->configured)
			return min(budget->current_ma, 500U) * 1000;
		return min(budget->current_ma, 100U) * 1000;
	}
	/* A proven brick is independent of USB suspend on an unenumerated port. */
	if (brick_ua > 0) {
		*type = POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID;
		return min(brick_ua, 2400000);
	}
	return 500000;
}

static void apple_tristar_auto_cfg(struct tristar_auto_cfg *cfg)
{
	*cfg = (struct tristar_auto_cfg){
		.probe_ms = 2000,
		.busy_probe_ms = 10000,
		.verify_ms = 5000,
		.settle_ms = 300,
		/* 250 ms polls: an unplug shows within 750 ms */
		.detach_polls = 3,
		.handshake_tries = 2,
		.input_ua = clamp_t(int, READ_ONCE(auto_input_ua), APPLE_TRISTAR_FLOOR_UA,
				    APPLE_TRISTAR_ADAPTER_MAX_UA),
		.floor_ua = APPLE_TRISTAR_FLOOR_UA,
	};
}

/* Caller holds ts->lock. Poll gaps can hide VBUS loss; ID/phase are evidence too. */
static void apple_tristar_new_source(struct apple_tristar *ts, const char *reason)
{
	ts->attach_id++;
	ts->epoch_reason = reason;
	ts->epoch_notify = true;
	ts->source_id_valid = false;
	ts->computer = false;
	atomic_set(&ts->computer_seen, 0);
	ts->brick_sampled = false;
	ts->brick_ua = 0;
	ts->bc_ua = 0;
	ts->bc_primary_mv = -ENODATA;
	ts->bc_secondary_mv = -ENODATA;
	ts->bc_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	ts->analog_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	ts->current_ua = APPLE_TRISTAR_FLOOR_UA;
	ts->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	ts->classify_after = jiffies + msecs_to_jiffies(300);
	ts->detect_attempts = 0;
	ts->detect_logged = false;
	ts->detect_failures = 0;
	ts->retry_ms = 0;
	ts->detect_error = 0;
	ts->detect_stage = reason;
	usb_gadget_current_source_detached(ts->usb_node);
}

/* First ID refines this source; a changed ID proves a new source epoch. */
static void apple_tristar_source_id(struct apple_tristar *ts, const u8 *id)
{
	bool changed = !ts->source_id_valid || memcmp(ts->source_id, id, sizeof(ts->source_id));

	if (ts->source_id_valid && changed)
		apple_tristar_new_source(ts, "digital-id");
	memcpy(ts->source_id, id, sizeof(ts->source_id));
	ts->source_id_valid = true;
	if (changed) {
		/* Preserve this epoch's BC evidence; once after ID settle, try CC too. */
		ts->brick_sampled = false;
		ts->classify_after = jiffies + msecs_to_jiffies(300);
		ts->detect_failures = 0;
		ts->retry_ms = 0;
	}
}

static void apple_tristar_source_phase(struct apple_tristar *ts, enum tristar_auto_phase old)
{
	if (old != TRISTAR_AUTO_OFF && old != TRISTAR_AUTO_EMPTY &&
	    old != ts->autop.phase &&
	    (ts->autop.phase == TRISTAR_AUTO_DEVICE || ts->autop.phase == TRISTAR_AUTO_EMPTY)) {
		apple_tristar_new_source(ts, "accessory-phase");
		tristar_auto_forget(&ts->autop);
	}
}

static int apple_tristar_update(struct apple_tristar *ts)
{
	struct usb_gadget_current_budget budget;
	struct tristar_auto_cfg cfg;
	int value, error, online, current_ua = 0, type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int old_current, old_type, auto_online, auto_ua;
	bool changed, accessory;
	/* Query before taking ts->lock: role changes unbind the gadget. */
	bool host = ts->role_sw &&
		usb_role_switch_get_role(ts->role_sw) == USB_ROLE_HOST;

	apple_tristar_auto_cfg(&cfg);

	mutex_lock(&ts->lock);
	if (ts->sleeping || ts->stopping) {
		mutex_unlock(&ts->lock);
		return -EHOSTDOWN;
	}
	old_current = ts->current_ua;
	old_type = ts->usb_type;
	value = apple_tristar_read(ts->client, TRISTAR_REVISION);
	error = value < 0 ? value : value == TRISTAR_J71T_REVISION ? 0 : -ENODEV;
	if (!error) {
		value = apple_tristar_read(ts->client, TRISTAR_VBUS);
		if (value < 0)
			error = value;
	}
	ts->status = error ?: value;
	/* A transport failure and USB reset/suspend are not source removal. */
	if (!error && ts->source_present && !(value & 3)) {
		ts->source_present = false;
		apple_tristar_new_source(ts, "vbus-detach");
		ts->detach_role = true;
		tristar_auto_forget(&ts->autop);
		ts->autop.phase = TRISTAR_AUTO_EMPTY;
		ts->autop.was_empty = true;
		ts->autop.empty = 0;
	}
	if (!error && (value & 3) == 3)
		ts->source_present = true;
	/*
	 * The camera adapter, once identified, is a power source in host mode
	 * too (iOS: USBPower Yes, 2100 mA after the handshake). Anything else
	 * in host mode is not a grant to charge from our own host port.
	 */
	accessory = !error && tristar_auto_supply(&ts->autop, &cfg, value, &auto_online, &auto_ua);
	/* IC getVBUSDetect(strict=1): both low bits, never orientation bits. */
	online = accessory ? auto_online : !host && !error && (value & 3) == 3;
	/* Route repair belongs to the role-serialized monitor, never a getter. */
	if (ts->restore_pending || ts->dcp_pending)
		error = -EUCLEAN;
	if (accessory && online && !error) {
		/* An Accessory Charger Adapter: power in while the iPad is the host. */
		current_ua = auto_ua;
		type = POWER_SUPPLY_USB_TYPE_ACA;
	} else if (online && !error) {
		/* Positive USB session evidence outranks every earlier analog result. */
		if (atomic_read(&ts->computer_seen)) {
			ts->computer = true;
			ts->brick_ua = 0;
		}
		if (usb_gadget_get_current_budget(ts->usb_node, &budget)) {
			current_ua = apple_tristar_budget(NULL, 0, &type);
			if (ts->computer) {
				type = POWER_SUPPLY_USB_TYPE_SDP;
				current_ua = APPLE_TRISTAR_FLOOR_UA;
			} else if (ts->brick_ua) {
				type = ts->analog_type;
				current_ua = ts->brick_ua;
			}
		} else {
			if (budget.connected || budget.configured || budget.host_grant) {
				atomic_set(&ts->computer_seen, 1);
				ts->computer = true;
				ts->brick_ua = 0;
			}
			current_ua = apple_tristar_budget(&budget, ts->brick_ua, &type);
			if (ts->brick_ua &&
			    ts->analog_type != POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID) {
				type = ts->analog_type;
				current_ua = ts->brick_ua;
			}
			if (ts->computer && !ts->brick_ua) {
				type = POWER_SUPPLY_USB_TYPE_SDP;
				current_ua = max(current_ua, APPLE_TRISTAR_FLOOR_UA);
			}
		}
	}
	changed = ts->error != error || ts->online != online || ts->epoch_notify;
	ts->epoch_notify = false;
	changed |= old_current != current_ua || old_type != type;
	ts->error = error;
	ts->online = online;
	ts->current_ua = current_ua;
	ts->usb_type = type;
	mutex_unlock(&ts->lock);
	if (changed && ts->psy)
		power_supply_changed(ts->psy);
	return error;
}

static int
apple_tristar_get_property(struct power_supply *psy,
			   enum power_supply_property prop,
			   union power_supply_propval *value)
{
	struct apple_tristar *ts = power_supply_get_drvdata(psy);
	int ret;

	/* Policy consumers get a live read rather than a stale cached cable. */
	ret = apple_tristar_update(ts);
	mutex_lock(&ts->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		value->intval = ts->online;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		value->intval = ts->current_ua;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		value->intval = ts->usb_type;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		value->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&ts->lock);
	return ret;
}

static void apple_tristar_auto_run(struct apple_tristar *ts);

static void apple_tristar_poll(struct work_struct *work)
{
	struct apple_tristar *ts = container_of(to_delayed_work(work),
						       struct apple_tristar, work);
	int ret;

	apple_tristar_update(ts);
	apple_tristar_auto_run(ts);
	ret = ts->role_sw ?
		apple_s8000_usb_role_monitor(ts->role_sw, apple_tristar_monitor, ts) :
		apple_tristar_monitor(ts);
	mutex_lock(&ts->lock);
	ts->monitor_error = ret;
	mutex_unlock(&ts->lock);
	apple_tristar_update(ts);
	mutex_lock(&ts->lock);
	if (!ts->stopping && !ts->sleeping)
		mod_delayed_work(system_power_efficient_wq, &ts->work, msecs_to_jiffies(250));
	mutex_unlock(&ts->lock);
}

static int apple_tristar_usb_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct apple_tristar *ts = container_of(nb, struct apple_tristar, usb_notifier);
	const struct usb_gadget_current_budget *budget = data;

	if (budget->of_node != ts->usb_node || READ_ONCE(ts->stopping) ||
	    READ_ONCE(ts->sleeping))
		return NOTIFY_DONE;
	/* Idle budget/disconnect notifications are not USB data sessions. */
	WRITE_ONCE(ts->usb_event_source, event);
	atomic_inc(&ts->usb_events);
	if (budget->connected || budget->configured || budget->host_grant) {
		atomic_set(&ts->usb_session, 1);
		/* Retain a short session even if it ends before the next worker. */
		atomic_set(&ts->computer_seen, 1);
		atomic_inc(&ts->reset_generation);
	} else if (atomic_xchg(&ts->usb_session, 0)) {
		/* A short real session ending between two samples still cancels. */
		atomic_inc(&ts->reset_generation);
	} else {
		atomic_inc(&ts->usb_idle_events);
	}
	mod_delayed_work(system_power_efficient_wq, &ts->work, 0);
	return NOTIFY_OK;
}

static int apple_tristar_quiesce(struct apple_tristar *ts)
{
	int attempt, ret = 0;

	mutex_lock(&ts->lock);
	WRITE_ONCE(ts->sleeping, true);
	mutex_unlock(&ts->lock);
	if (ts->notifier_registered) {
		usb_gadget_unregister_current_notifier(&ts->usb_notifier);
		ts->notifier_registered = false;
	}
	cancel_delayed_work_sync(&ts->work);
	mutex_lock(&ts->lock);
	ret = ts->restore_pending || ts->dcp_pending;
	mutex_unlock(&ts->lock);
	if (!ret)
		return 0;
	for (attempt = 0; attempt < 3; attempt++) {
		ret = ts->role_sw ?
			apple_s8000_usb_role_monitor(ts->role_sw, apple_tristar_monitor, ts) :
			apple_tristar_monitor(ts);
		if (!ret)
			break;
		msleep(20);
	}
	if (ret)
		dev_crit(&ts->client->dev, "Tristar route restore failed at quiesce: %d\n", ret);
	return ret;
}

static void apple_tristar_stop(void *data)
{
	struct apple_tristar *ts = data;

	mutex_lock(&ts->lock);
	WRITE_ONCE(ts->stopping, true);
	mutex_unlock(&ts->lock);
	apple_tristar_quiesce(ts);
}

static int apple_tristar_resume(struct device *dev)
{
	struct apple_tristar *ts = dev_get_drvdata(dev);
	int ret;

	ret = usb_gadget_register_current_notifier(&ts->usb_notifier);
	if (ret && ret != -EOPNOTSUPP)
		return ret;
	ts->notifier_registered = !ret;
	mutex_lock(&ts->lock);
	ts->classify_after = jiffies + msecs_to_jiffies(1000);
	WRITE_ONCE(ts->sleeping, false);
	mod_delayed_work(system_power_efficient_wq, &ts->work, 0);
	mutex_unlock(&ts->lock);
	return 0;
}

static int apple_tristar_suspend(struct device *dev)
{
	struct apple_tristar *ts = dev_get_drvdata(dev);
	int ret = apple_tristar_quiesce(ts);

	if (ret)
		apple_tristar_resume(dev);
	return ret;
}

static void apple_tristar_shutdown(struct i2c_client *client)
{
	apple_tristar_stop(i2c_get_clientdata(client));
}

static DEFINE_SIMPLE_DEV_PM_OPS(apple_tristar_pm_ops,
			       apple_tristar_suspend, apple_tristar_resume);

/*
 * Lightning ID-bus: manual, debugfs-triggered GetID, link recovery and
 * power handshake. Nothing here runs on its own and nothing changes the
 * USB role.
 */

#define APPLE_TRISTAR_PH_WINDOW_MS 30000

static int apple_tristar_idbus_read(void *ctx, u8 reg)
{
	struct apple_tristar *ts = ctx;

	return apple_tristar_read(ts->client, reg);
}

static int apple_tristar_idbus_write(void *ctx, u8 reg, u8 val)
{
	struct apple_tristar *ts = ctx;

	return i2c_smbus_write_byte_data(ts->client, reg, val);
}

/* Hold the bus across the read and write: 0x05 is shared with the role switch. */
static int apple_tristar_idbus_update(void *ctx, u8 reg, u8 clear, u8 set)
{
	struct apple_tristar *ts = ctx;
	struct i2c_client *client = ts->client;
	union i2c_smbus_data data;
	u8 old = 0;
	int ret;

	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);
	ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			       I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE, NULL);
	if (!ret)
		ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				       I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
	if (!ret) {
		old = data.byte;
		data.byte = (old & ~clear) | set;
		ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				       I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &data);
	}
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	return ret ?: old;
}

/*
 * iOS operation 1 with an N-byte result: pointer write + STOP, then a
 * separate read. PA Semi offers no SMBus I2C-block transfer, so use
 * plain messages, each ending in STOP.
 */
static int apple_tristar_idbus_block_read(void *ctx, u8 reg, u8 *buf, unsigned int len)
{
	struct apple_tristar *ts = ctx;
	struct i2c_client *client = ts->client;
	struct i2c_msg sel = {
		.addr = client->addr,
		.flags = client->flags & I2C_M_TEN,
		.len = 1,
		.buf = &reg,
	};
	struct i2c_msg rx = {
		.addr = client->addr,
		.flags = (client->flags & I2C_M_TEN) | I2C_M_RD,
		.len = len,
		.buf = buf,
	};
	int ret;

	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(client->adapter, &sel, 1);
	if (ret == 1)
		ret = __i2c_transfer(client->adapter, &rx, 1);
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

/* iOS operation 2: subaddress and data in one write message. */
static int apple_tristar_idbus_block_write(void *ctx, u8 reg, const u8 *buf, unsigned int len)
{
	struct apple_tristar *ts = ctx;
	u8 msg[1 + TRISTAR_IDBUS_MAX_FRAME];
	int ret;

	if (len > TRISTAR_IDBUS_MAX_FRAME)
		return -EINVAL;
	msg[0] = reg;
	memcpy(msg + 1, buf, len);
	ret = i2c_master_send(ts->client, msg, len + 1);
	return ret == (int)len + 1 ? 0 : ret < 0 ? ret : -EIO;
}

static void apple_tristar_idbus_sleep(void *ctx, unsigned int ms)
{
	fsleep(ms * USEC_PER_MSEC);
}

/* Every line goes to the kernel log and to the run's debugfs transcript. */
static __printf(2, 3) void apple_tristar_idbus_log(void *ctx, const char *fmt, ...)
{
	struct apple_tristar *ts = ctx;
	struct apple_tristar_idbus *st = ts->idbus;
	struct va_format vaf;
	va_list args;

	if (!st->quiet) {
		va_start(args, fmt);
		vaf.fmt = fmt;
		vaf.va = &args;
		dev_info(&ts->client->dev, "idbus: %pV\n", &vaf);
		va_end(args);
	}
	if (st->log_len + 2 >= st->log_size)
		return;
	va_start(args, fmt);
	st->log_len += vscnprintf(st->log + st->log_len, st->log_size - st->log_len - 1,
				  fmt, args);
	va_end(args);
	st->log[st->log_len++] = '\n';
	st->log[st->log_len] = '\0';
}

static const struct tristar_idbus_ops apple_tristar_idbus_ops = {
	.read = apple_tristar_idbus_read,
	.write = apple_tristar_idbus_write,
	.update = apple_tristar_idbus_update,
	.block_read = apple_tristar_idbus_block_read,
	.block_write = apple_tristar_idbus_block_write,
	.sleep_ms = apple_tristar_idbus_sleep,
	.log = apple_tristar_idbus_log,
};

/* Caller holds ts->lock. */
static void apple_tristar_idbus_begin(struct apple_tristar *ts, char *log, size_t size)
{
	struct apple_tristar_idbus *st = ts->idbus;

	st->log = log;
	st->log_size = size;
	st->log_len = 0;
	log[0] = '\0';
}

/*
 * Manual (debugfs) or automatic (accessory policy, quiet) GetID. On success
 * the ID is copied to @id if given.
 */
static int apple_tristar_identify(struct apple_tristar *ts, unsigned int flags, bool quiet,
				  u8 *id)
{
	struct apple_tristar_idbus *st = ts->idbus;
	/* Query before taking ts->lock, as apple_tristar_update() does. */
	enum usb_role role = ts->role_sw ? usb_role_switch_get_role(ts->role_sw) : USB_ROLE_NONE;
	int ret;

	mutex_lock(&ts->lock);
	apple_tristar_idbus_begin(ts, st->id_log, sizeof(st->id_log));
	memset(&st->id, 0, sizeof(st->id));
	st->id_run = true;
	st->quiet = quiet;
	if (ts->sleeping || ts->stopping) {
		apple_tristar_idbus_log(ts, "identify refused: supplier stopped");
		ret = -EHOSTDOWN;
	} else if (!i2c_check_functionality(ts->client->adapter, I2C_FUNC_I2C)) {
		apple_tristar_idbus_log(ts, "identify refused: adapter lacks plain I2C messages");
		ret = -EOPNOTSUPP;
	} else {
		apple_tristar_idbus_log(ts, "%s identify, USB role %s",
					quiet ? "automatic" : "manual", usb_role_string(role));
		ret = tristar_idbus_identify(&st->ib, flags, &st->id);
		st->id_at = jiffies;
		if (!ret && !st->id.id_valid)
			ret = -ENODATA;
	}
	st->id.status = ret;
	st->quiet = false;
	if (!ret && id)
		memcpy(id, st->id.id, TRISTAR_IDBUS_ID_LEN);
	mutex_unlock(&ts->lock);
	return ret;
}

/* @policy: the accessory policy's own run, which allow_power_handshake does not gate. */
static int apple_tristar_power_handshake(struct apple_tristar *ts, bool policy)
{
	struct apple_tristar_idbus *st = ts->idbus;
	int ret;

	mutex_lock(&ts->lock);
	apple_tristar_idbus_begin(ts, st->ph_log, sizeof(st->ph_log));
	memset(&st->ph, 0, sizeof(st->ph));
	st->ph_run = true;
	if (policy)
		apple_tristar_idbus_log(ts, "power handshake for the identified camera adapter");
	if (!policy && !READ_ONCE(allow_power_handshake)) {
		apple_tristar_idbus_log(ts, "power handshake refused: allow_power_handshake=0");
		ret = -EPERM;
	} else if (ts->sleeping || ts->stopping) {
		apple_tristar_idbus_log(ts, "power handshake refused: supplier stopped");
		ret = -EHOSTDOWN;
	} else if (!st->id.id_valid ||
		   time_after(jiffies, st->id_at + msecs_to_jiffies(APPLE_TRISTAR_PH_WINDOW_MS))) {
		apple_tristar_idbus_log(ts, "power handshake refused: no ID verified in %u s",
					APPLE_TRISTAR_PH_WINDOW_MS / 1000);
		ret = -ENOLINK;
	} else {
		ret = tristar_idbus_power_handshake(&st->ib, &st->ph);
	}
	st->ph.status = ret;
	mutex_unlock(&ts->lock);
	return ret;
}

static int apple_tristar_recover(struct apple_tristar *ts, unsigned int action)
{
	struct apple_tristar_idbus *st = ts->idbus;
	int ret;

	mutex_lock(&ts->lock);
	apple_tristar_idbus_begin(ts, st->rc_log, sizeof(st->rc_log));
	memset(&st->rc, 0, sizeof(st->rc));
	st->rc_run = true;
	if (ts->sleeping || ts->stopping) {
		apple_tristar_idbus_log(ts, "recover refused: supplier stopped");
		ret = -EHOSTDOWN;
	} else if (!i2c_check_functionality(ts->client->adapter, I2C_FUNC_I2C)) {
		apple_tristar_idbus_log(ts, "recover refused: adapter lacks plain I2C messages");
		ret = -EOPNOTSUPP;
	} else {
		ret = tristar_idbus_recover(&st->ib, action, &st->rc);
	}
	st->rc.status = ret;
	mutex_unlock(&ts->lock);
	return ret;
}

/*
 * Lightning accessory policy (apple_tristar_auto.h): the iOS mode-2 path.
 * GetID on plug-in and periodically while no USB host uses the gadget; for
 * the camera adapter, the power handshake and then the host role, kept
 * until the adapter is unplugged.
 */

/* Caller holds ts->lock. Logs phase changes and the given event. */
static __printf(3, 4) void apple_tristar_auto_note(struct apple_tristar *ts,
						    enum tristar_auto_phase old,
						    const char *fmt, ...)
{
	enum tristar_auto_phase phase = ts->autop.phase;
	char msg[96] = "", line[192];
	size_t len, drop;
	va_list args;

	if (old == phase && !fmt)
		return;
	if (fmt) {
		va_start(args, fmt);
		vscnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);
	}
	if (old == phase)
		len = scnprintf(line, sizeof(line), "%llu ms %s: %s",
				ktime_to_ms(ktime_get_boottime()),
				tristar_auto_phase_name(phase), msg);
	else
		len = scnprintf(line, sizeof(line), "%llu ms %s -> %s%s%s",
				ktime_to_ms(ktime_get_boottime()),
				tristar_auto_phase_name(old), tristar_auto_phase_name(phase),
				*msg ? ": " : "", msg);
	dev_info(&ts->client->dev, "lightning: %s\n", line);
	/* Keep the newest lines: drop whole lines from the front. */
	while (ts->auto_log_len + len + 2 > sizeof(ts->auto_log)) {
		char *nl = memchr(ts->auto_log, '\n', ts->auto_log_len);

		drop = nl ? nl - ts->auto_log + 1 : ts->auto_log_len;
		memmove(ts->auto_log, ts->auto_log + drop, ts->auto_log_len - drop);
		ts->auto_log_len -= drop;
	}
	memcpy(ts->auto_log + ts->auto_log_len, line, len);
	ts->auto_log_len += len;
	ts->auto_log[ts->auto_log_len++] = '\n';
	ts->auto_log[ts->auto_log_len] = '\0';
}

static enum tristar_auto_action
apple_tristar_auto_act(struct apple_tristar *ts, const struct tristar_auto_cfg *cfg,
		       enum tristar_auto_action action)
{
	struct tristar_auto *ta = &ts->autop;
	u8 id[TRISTAR_IDBUS_ID_LEN] = {};
	enum tristar_auto_phase old;
	bool had, same;
	int ret;

	switch (action) {
	case TRISTAR_AUTO_PROBE:
		/* No reorient: it can move a live Mac tether to the other lanes. */
		ret = apple_tristar_identify(ts, TRISTAR_IDBUS_NO_REORIENT, true, id);
		mutex_lock(&ts->lock);
		old = ta->phase;
		had = ta->id_valid;
		same = had && !ret && !memcmp(ta->id, id, sizeof(id));
		if (!ret)
			apple_tristar_source_id(ts, id);
		action = tristar_auto_probed(ta, cfg, ktime_to_ms(ktime_get_boottime()), ret, id);
		if (!ret && !same)
			apple_tristar_auto_note(ts, old, "digital ID %*ph%s", (int)sizeof(id), id,
						ta->adapter ? " (camera adapter)" : "");
		else
			apple_tristar_auto_note(ts, old, NULL);
		mutex_unlock(&ts->lock);
		return action;
	case TRISTAR_AUTO_POWER_HANDSHAKE:
		ret = apple_tristar_power_handshake(ts, true);
		mutex_lock(&ts->lock);
		old = ta->phase;
		action = tristar_auto_handshake_done(ta, cfg, ret);
		apple_tristar_auto_note(ts, old, "power handshake %s (%d), try %u",
					ret ? "failed" : "ok", ret, ta->ph_tries);
		mutex_unlock(&ts->lock);
		break;
	case TRISTAR_AUTO_TO_HOST:
		ret = apple_s8000_usb_role_claim(ts->role_sw, true);
		if (!ret)
			ret = usb_role_switch_set_role(ts->role_sw, USB_ROLE_HOST);
		if (ret)
			apple_s8000_usb_role_claim(ts->role_sw, false);
		mutex_lock(&ts->lock);
		old = ta->phase;
		tristar_auto_role_done(ta, cfg, ktime_to_ms(ktime_get_boottime()), true, ret);
		apple_tristar_auto_note(ts, old, "host role %s (%d)", ret ? "failed" : "on", ret);
		mutex_unlock(&ts->lock);
		action = TRISTAR_AUTO_NONE;
		break;
	case TRISTAR_AUTO_TO_DEVICE:
		ret = 0;
		if (apple_s8000_usb_role_accessory(ts->role_sw))
			ret = usb_role_switch_set_role(ts->role_sw, USB_ROLE_DEVICE);
		mutex_lock(&ts->lock);
		old = ta->phase;
		tristar_auto_role_done(ta, cfg, ktime_to_ms(ktime_get_boottime()), false, ret);
		apple_tristar_auto_note(ts, old, "device role %s (%d)", ret ? "failed" : "on", ret);
		mutex_unlock(&ts->lock);
		action = TRISTAR_AUTO_NONE;
		break;
	case TRISTAR_AUTO_NONE:
		return action;
	}
	/* The input budget follows the handshake and the role; notifies on change. */
	apple_tristar_update(ts);
	return action;
}

static void apple_tristar_auto_run(struct apple_tristar *ts)
{
	struct usb_gadget_current_budget budget;
	struct tristar_auto_cfg cfg;
	struct tristar_auto_in in = { .enabled = READ_ONCE(auto_host) };
	enum tristar_auto_action action;
	enum tristar_auto_phase old;
	unsigned int i;

	if (!ts->role_sw || !ts->idbus)
		return;
	apple_tristar_auto_cfg(&cfg);
	mutex_lock(&ts->lock);
	if (ts->detach_role) {
		ts->detach_role = false;
		mutex_unlock(&ts->lock);
		apple_tristar_auto_act(ts, &cfg, TRISTAR_AUTO_TO_DEVICE);
	} else {
		mutex_unlock(&ts->lock);
	}
	/* Role and gadget queries before ts->lock, as apple_tristar_update() does. */
	in.host = usb_role_switch_get_role(ts->role_sw) == USB_ROLE_HOST;
	in.owned = in.host && apple_s8000_usb_role_accessory(ts->role_sw);
	in.link_active = !usb_gadget_get_current_budget(ts->usb_node, &budget) &&
			 budget.configured && !budget.suspended;
	mutex_lock(&ts->lock);
	if (ts->sleeping || ts->stopping) {
		mutex_unlock(&ts->lock);
		return;
	}
	in.now = ktime_to_ms(ktime_get_boottime());
	in.status = ts->status;
	old = ts->autop.phase;
	action = tristar_auto_step(&ts->autop, &cfg, &in);
	apple_tristar_source_phase(ts, old);
	apple_tristar_auto_note(ts, old, NULL);
	mutex_unlock(&ts->lock);
	/* A probe can lead to the handshake and then the role switch. */
	for (i = 0; i < 4 && action != TRISTAR_AUTO_NONE; i++)
		action = apple_tristar_auto_act(ts, &cfg, action);
}

/* "1" or "identify", optionally with "swap" and/or "noreorient". */
static int apple_tristar_parse_trigger(const char __user *ubuf, size_t count,
				       unsigned int *flags)
{
	char buf[48], *cur, *tok;
	bool go = false;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	*flags = 0;
	cur = buf;
	while ((tok = strsep(&cur, " \t\n"))) {
		if (!*tok)
			continue;
		if (!strcmp(tok, "1") || !strcmp(tok, "identify"))
			go = true;
		else if (!strcmp(tok, "swap"))
			*flags |= TRISTAR_IDBUS_SWAP_HOSTID;
		else if (!strcmp(tok, "noreorient"))
			*flags |= TRISTAR_IDBUS_NO_REORIENT;
		else
			return -EINVAL;
	}
	return go ? 0 : -EINVAL;
}

static ssize_t apple_tristar_identify_write(struct file *file, const char __user *ubuf,
					    size_t count, loff_t *ppos)
{
	unsigned int flags;
	int ret;

	ret = apple_tristar_parse_trigger(ubuf, count, &flags);
	if (!ret)
		ret = apple_tristar_identify(file->private_data, flags, false, NULL);
	if (ret)
		return ret;
	return count;
}

static ssize_t apple_tristar_power_handshake_write(struct file *file, const char __user *ubuf,
						   size_t count, loff_t *ppos)
{
	unsigned int flags;
	int ret;

	ret = apple_tristar_parse_trigger(ubuf, count, &flags);
	if (!ret && flags)
		ret = -EINVAL;
	if (!ret)
		ret = apple_tristar_power_handshake(file->private_data, false);
	if (ret)
		return ret;
	return count;
}

/* Exactly one of "reorient", "idbus_reset" or "fault_ack". */
static ssize_t apple_tristar_recover_write(struct file *file, const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	static const char *const verbs[] = {
		[TRISTAR_IDBUS_RECOVER_REORIENT] = "reorient",
		[TRISTAR_IDBUS_RECOVER_RESET] = "idbus_reset",
		[TRISTAR_IDBUS_RECOVER_FAULT_ACK] = "fault_ack",
	};
	char buf[16];
	int action, ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	action = sysfs_match_string(verbs, buf);
	if (action < 0)
		return action;
	ret = apple_tristar_recover(file->private_data, action);
	if (ret)
		return ret;
	return count;
}

static const struct file_operations apple_tristar_recover_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = apple_tristar_recover_write,
	.llseek = noop_llseek,
};

static const struct file_operations apple_tristar_identify_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = apple_tristar_identify_write,
	.llseek = noop_llseek,
};

static const struct file_operations apple_tristar_power_handshake_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = apple_tristar_power_handshake_write,
	.llseek = noop_llseek,
};

static int apple_tristar_identify_log_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_puts(s, ts->idbus->id_run ? ts->idbus->id_log : "identify has not run\n");
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_identify_log);

static int apple_tristar_power_handshake_log_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_puts(s, ts->idbus->ph_run ? ts->idbus->ph_log : "power handshake has not run\n");
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_power_handshake_log);

static int apple_tristar_recover_log_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_puts(s, ts->idbus->rc_run ? ts->idbus->rc_log : "recover has not run\n");
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_recover_log);

static int apple_tristar_digital_id_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;
	struct tristar_idbus_id_result *id = &ts->idbus->id;
	char hex[3 * TRISTAR_IDBUS_MAX_FRAME + 1];

	mutex_lock(&ts->lock);
	if (id->id_valid) {
		tristar_idbus_format_hex(id->id, TRISTAR_IDBUS_ID_LEN, hex, sizeof(hex));
		seq_printf(s, "%s\n", hex);
	} else if (ts->idbus->id_run) {
		seq_printf(s, "none (status %d)\n", id->status);
	} else {
		seq_puts(s, "none (identify has not run)\n");
	}
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_digital_id);

static int apple_tristar_digital_id_decoded_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;
	struct tristar_idbus_id_result *id = &ts->idbus->id;
	char text[160];

	mutex_lock(&ts->lock);
	if (id->id_valid) {
		tristar_idbus_format_fields(id->id, text, sizeof(text));
		seq_printf(s, "%s\n", text);
	} else {
		seq_puts(s, "none\n");
	}
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_digital_id_decoded);

/* Raw evidence, published even when the header or CRC was wrong. */
static int apple_tristar_idbus_frame_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;
	struct tristar_idbus_id_result *id = &ts->idbus->id;
	char hex[3 * TRISTAR_IDBUS_MAX_FRAME + 1];

	mutex_lock(&ts->lock);
	if (id->frame_len) {
		tristar_idbus_format_hex(id->frame, id->frame_len, hex, sizeof(hex));
		seq_printf(s, "frame: %s (%s), source %s, 0x0c 0x%02x\n", hex,
			   tristar_idbus_verdict(id->frame_status),
			   id->autonomous ? "reorient-armed exchange" : "GetID command",
			   id->orientation);
	} else {
		seq_puts(s, "frame: none\n");
	}
	tristar_idbus_format_hex(id->retained, TRISTAR_IDBUS_MAX_FRAME, hex, sizeof(hex));
	seq_printf(s, "retained before GetID: %s (%s)\n", hex,
		   id->retained_ok ? "valid ID frame, may be stale" : "not a valid ID frame");
	if (id->reorient_read) {
		tristar_idbus_format_hex(id->reorient_frame, TRISTAR_IDBUS_MAX_FRAME, hex,
					 sizeof(hex));
		seq_printf(s, "after reorient: %s (%s), 0x0d events 0x%02x\n", hex,
			   tristar_idbus_verdict(id->reorient_status), id->reorient_events);
	}
	seq_printf(s, "status %d, GetID commands %u, reoriented %d, reset released %d, flags 0x%x\n",
		   id->status, id->attempts, id->reoriented, id->released_reset, id->flags);
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_idbus_frame);

/* Values cached from the last run; reading these never touches the bus. */
static int apple_tristar_reg_05_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_printf(s, "0x%02x\n", ts->idbus->ib.last_ctrl);
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_reg_05);

static int apple_tristar_reg_0d_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_printf(s, "0x%02x (seen this run 0x%02x)\n", ts->idbus->ib.last_event,
		   ts->idbus->ib.events);
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_reg_0d);

static int apple_tristar_reg_0a_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_printf(s, "0x%02x (acknowledged %u)\n", ts->idbus->ib.last_fault,
		   ts->idbus->ib.faults);
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_reg_0a);

static int apple_tristar_auto_log_show(struct seq_file *s, void *unused)
{
	struct apple_tristar *ts = s->private;

	mutex_lock(&ts->lock);
	seq_puts(s, ts->auto_log_len ? ts->auto_log : "no accessory policy events\n");
	mutex_unlock(&ts->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_tristar_auto_log);

static const char *apple_tristar_usb_type_name(int type)
{
	switch (type) {
	case POWER_SUPPLY_USB_TYPE_SDP:
		return "SDP";
	case POWER_SUPPLY_USB_TYPE_DCP:
		return "DCP";
	case POWER_SUPPLY_USB_TYPE_CDP:
		return "CDP";
	case POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID:
		return "BrickID";
	case POWER_SUPPLY_USB_TYPE_ACA:
		return "ACA";
	case POWER_SUPPLY_USB_TYPE_C:
		return "C";
	default:
		return "Unknown";
	}
}

static int apple_tristar_psy_int(const char *name, enum power_supply_property prop, int *val)
{
	union power_supply_propval value;
	struct power_supply *psy = power_supply_get_by_name(name);
	int ret;

	if (!psy)
		return -ENODEV;
	ret = power_supply_get_property(psy, prop, &value);
	power_supply_put(psy);
	if (!ret)
		*val = value.intval;
	return ret;
}

static const char *apple_tristar_status_name(int status)
{
	switch (status) {
	case POWER_SUPPLY_STATUS_CHARGING:
		return "Charging";
	case POWER_SUPPLY_STATUS_DISCHARGING:
		return "Discharging";
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
		return "Not charging";
	case POWER_SUPPLY_STATUS_FULL:
		return "Full";
	default:
		return "Unknown";
	}
}

static ssize_t apple_tristar_emit_psy(char *buf, ssize_t len, const char *name,
				      enum power_supply_property prop, const char *label)
{
	int val = 0, ret = apple_tristar_psy_int(name, prop, &val);

	if (ret)
		return len + sysfs_emit_at(buf, len, " %s=err%d", label, ret);
	if (prop == POWER_SUPPLY_PROP_STATUS)
		return len + sysfs_emit_at(buf, len, " %s=%s", label,
					   apple_tristar_status_name(val));
	return len + sysfs_emit_at(buf, len, " %s=%d", label, val);
}

/*
 * One world-readable snapshot for test tooling: policy phase, role, ID,
 * handshake, what this supplier reports and what the charger and the
 * battery see. No GetID or register write; the charger's ONLINE read
 * refreshes this supplier as any consumer's would.
 */
static ssize_t lightning_state_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct apple_tristar *ts = dev_get_drvdata(dev);
	struct tristar_auto ta;
	int status, online, current_ua, type, error;
	enum usb_role role = USB_ROLE_NONE;
	bool owned = false;
	ssize_t len = 0;

	if (ts->role_sw) {
		role = usb_role_switch_get_role(ts->role_sw);
		owned = apple_s8000_usb_role_accessory(ts->role_sw);
	}
	mutex_lock(&ts->lock);
	ta = ts->autop;
	status = ts->status;
	online = ts->online;
	current_ua = ts->current_ua;
	type = ts->usb_type;
	error = ts->error;
	len += sysfs_emit_at(buf, len,
			     "charger_detect: stage=%s pin=%s error=%d monitor_error=%d attempts=%u failures=%u retry_ms=%u poisoned=%d route=%d adc_status=%d dp_mv=%d dm_mv=%d cc_mv=%d sampled=%d poison_stage=%s poison_attach=%u\n",
			     ts->detect_stage ?: "none", ts->detect_pin ?: "none",
			     ts->detect_error, ts->monitor_error, ts->detect_attempts,
			     ts->detect_failures, ts->retry_ms, ts->adc_poisoned,
			     ts->route_before, ts->adc_status, ts->dp_mv, ts->dm_mv,
			     ts->cc_mv, ts->brick_sampled, ts->adc_fault_stage ?: "none",
			     ts->adc_fault_attach);
	len += sysfs_emit_at(buf, len,
			     "source_epoch: id=%u reason=%s bc_ua=%d bc_type=%d cc_first_mv=%d usb_events=%d usb_idle_events=%d usb_generation=%d usb_event_source=%lu computer_seen=%d bc_primary_mv=%d bc_secondary_mv=%d\n",
			     ts->attach_id, ts->epoch_reason ?: "boot", ts->bc_ua, ts->bc_type,
			     ts->cc_first_mv, atomic_read(&ts->usb_events),
			     atomic_read(&ts->usb_idle_events), atomic_read(&ts->reset_generation),
			     READ_ONCE(ts->usb_event_source), atomic_read(&ts->computer_seen),
			     ts->bc_primary_mv, ts->bc_secondary_mv);
	mutex_unlock(&ts->lock);

	len += sysfs_emit_at(buf, len, "auto_host: %d\nphase: %s\nrole: %s%s\n",
			     READ_ONCE(auto_host), tristar_auto_phase_name(ta.phase),
			     usb_role_string(role), owned ? " (accessory)" : "");
	if (status < 0)
		len += sysfs_emit_at(buf, len, "status_0c: error %d\n", status);
	else
		len += sysfs_emit_at(buf, len,
				     "status_0c: 0x%02x (vbus %d, present %d, orientation %d)\n",
				     status, (status & TRISTAR_AUTO_VBUS) == TRISTAR_AUTO_VBUS,
				     !!(status & TRISTAR_AUTO_PRESENT),
				     !!(status & TRISTAR_AUTO_ORIENTATION));
	if (ta.id_valid)
		len += sysfs_emit_at(buf, len, "digital_id: %*ph%s\n", (int)sizeof(ta.id), ta.id,
				     ta.adapter ? " (camera adapter)" : "");
	else
		len += sysfs_emit_at(buf, len, "digital_id: none (last GetID %d)\n", ta.id_status);
	if (ta.ph_status == 1)
		len += sysfs_emit_at(buf, len, "power_handshake: not run\n");
	else
		len += sysfs_emit_at(buf, len, "power_handshake: %s (%d, tries %u)\n",
				     ta.ph_ok ? "ok" : "failed", ta.ph_status, ta.ph_tries);
	len += sysfs_emit_at(buf, len,
			     "supply: online %d, type %s, current_max_ua %d, error %d\n",
			     online, apple_tristar_usb_type_name(type), current_ua, error);
	/* Not under ts->lock: the charger reads this supplier. */
	len += sysfs_emit_at(buf, len, "charger:");
	len = apple_tristar_emit_psy(buf, len, "charger", POWER_SUPPLY_PROP_ONLINE, "online");
	len = apple_tristar_emit_psy(buf, len, "charger", POWER_SUPPLY_PROP_STATUS, "status");
	len = apple_tristar_emit_psy(buf, len, "charger", POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
				     "input_current_limit_ua");
	len += sysfs_emit_at(buf, len, "\nbattery:");
	len = apple_tristar_emit_psy(buf, len, "battery", POWER_SUPPLY_PROP_STATUS, "status");
	len = apple_tristar_emit_psy(buf, len, "battery", POWER_SUPPLY_PROP_CURRENT_NOW,
				     "current_now_ua");
	len = apple_tristar_emit_psy(buf, len, "battery", POWER_SUPPLY_PROP_VOLTAGE_NOW,
				     "voltage_now_uv");
	len = apple_tristar_emit_psy(buf, len, "battery", POWER_SUPPLY_PROP_CAPACITY, "capacity");
	len += sysfs_emit_at(buf, len,
			     "\ncounters: probes %u probe_errors %u host_entries %u detaches %u inhibits %u role_errors %u\n",
			     ta.probes, ta.probe_errors, ta.host_entries, ta.detaches, ta.inhibits,
			     ta.role_errors);
	return len;
}
static DEVICE_ATTR_RO(lightning_state);

static struct attribute *apple_tristar_attrs[] = {
	&dev_attr_lightning_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(apple_tristar);

static void apple_tristar_remove_debugfs(void *data)
{
	struct apple_tristar *ts = data;

	/* Waits for an in-flight identify before the supplier stops. */
	debugfs_remove_recursive(ts->debugfs);
}

/* Optional: an ID-bus setup failure never blocks the supplier. */
static void apple_tristar_debugfs_init(struct apple_tristar *ts)
{
	struct device *dev = &ts->client->dev;
	struct dentry *dir;

	ts->idbus = devm_kzalloc(dev, sizeof(*ts->idbus), GFP_KERNEL);
	if (!ts->idbus)
		return;
	ts->idbus->ib.ops = &apple_tristar_idbus_ops;
	ts->idbus->ib.ctx = ts;
	dir = debugfs_create_dir("apple-tristar", NULL);
	ts->debugfs = dir;
	debugfs_create_file("identify", 0200, dir, ts, &apple_tristar_identify_fops);
	debugfs_create_file("identify_log", 0444, dir, ts, &apple_tristar_identify_log_fops);
	debugfs_create_file("digital_id", 0444, dir, ts, &apple_tristar_digital_id_fops);
	debugfs_create_file("digital_id_decoded", 0444, dir, ts,
			    &apple_tristar_digital_id_decoded_fops);
	debugfs_create_file("idbus_frame", 0444, dir, ts, &apple_tristar_idbus_frame_fops);
	debugfs_create_file("reg_05", 0444, dir, ts, &apple_tristar_reg_05_fops);
	debugfs_create_file("reg_0d", 0444, dir, ts, &apple_tristar_reg_0d_fops);
	debugfs_create_file("reg_0a", 0444, dir, ts, &apple_tristar_reg_0a_fops);
	debugfs_create_file("power_handshake", 0200, dir, ts,
			    &apple_tristar_power_handshake_fops);
	debugfs_create_file("power_handshake_log", 0444, dir, ts,
			    &apple_tristar_power_handshake_log_fops);
	debugfs_create_file("recover", 0200, dir, ts, &apple_tristar_recover_fops);
	debugfs_create_file("recover_log", 0444, dir, ts, &apple_tristar_recover_log_fops);
	debugfs_create_file("auto_log", 0444, dir, ts, &apple_tristar_auto_log_fops);
	if (devm_add_action_or_reset(dev, apple_tristar_remove_debugfs, ts))
		dev_warn(dev, "ID-bus debug interface removed\n");
}

static void apple_tristar_put_device(void *data)
{
	put_device(data);
}

static void apple_tristar_put_node(void *data)
{
	of_node_put(data);
}

static const enum power_supply_property apple_tristar_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc apple_tristar_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID) | BIT(POWER_SUPPLY_USB_TYPE_ACA) |
		     BIT(POWER_SUPPLY_USB_TYPE_C),
	.properties = apple_tristar_properties,
	.num_properties = ARRAY_SIZE(apple_tristar_properties),
	.get_property = apple_tristar_get_property,
};

/**
 * apple_tristar_attach_id - snapshot the physical source epoch
 * @psy: Tristar supplier, held by the consumer
 * @id: epoch incremented on each observed VBUS removal
 *
 * Return: 0, or a live transport error. Link PM never increments the epoch.
 */
int apple_tristar_attach_id(struct power_supply *psy, unsigned int *id)
{
	struct apple_tristar *ts;
	int ret;

	if (psy->desc != &apple_tristar_desc)
		return -ENODEV;
	ts = power_supply_get_drvdata(psy);
	ret = apple_tristar_update(ts);
	if (ret)
		return ret;
	mutex_lock(&ts->lock);
	*id = ts->attach_id;
	mutex_unlock(&ts->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_tristar_attach_id);

static void apple_tristar_put_role(void *data)
{
	usb_role_switch_put(data);
}

static int apple_tristar_probe(struct i2c_client *client)
{
	struct power_supply_config config = {};
	struct apple_tristar *ts;
	struct device_node *node;
	struct i2c_client *pmic_client;
	struct platform_device *usb_device;
	struct device_link *link;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE))
		return -EOPNOTSUPP;
	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	ts->status = -ENODATA;
	ts->route_before = -ENODATA;
	ts->adc_status = -ENODATA;
	ts->dp_mv = -ENODATA;
	ts->dm_mv = -ENODATA;
	ts->cc_mv = -ENODATA;
	ts->bc_primary_mv = -ENODATA;
	ts->bc_secondary_mv = -ENODATA;
	ts->detect_stage = "initial";
	tristar_auto_init(&ts->autop);
	i2c_set_clientdata(client, ts);
	node = of_parse_phandle(client->dev.of_node, "apple,usb-role-switch", 0);
	if (node) {
		ts->role_sw = usb_role_switch_find_by_fwnode(of_fwnode_handle(node));
		of_node_put(node);
		if (!ts->role_sw)
			return -EPROBE_DEFER;
		ret = devm_add_action_or_reset(&client->dev, apple_tristar_put_role,
					       ts->role_sw);
		if (ret)
			return ret;
	}
	ts->usb_node = of_parse_phandle(client->dev.of_node, "usb-controller", 0);
	if (!ts->usb_node)
		return -EINVAL;
	ret = devm_add_action_or_reset(&client->dev, apple_tristar_put_node, ts->usb_node);
	if (ret)
		return ret;
	node = of_parse_phandle(client->dev.of_node, "apple,pmic", 0);
	if (!node)
		return -EINVAL;
	pmic_client = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!pmic_client)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(&client->dev, apple_tristar_put_device, &pmic_client->dev);
	if (ret)
		return ret;
	ts->pmic = dev_get_regmap(&pmic_client->dev, NULL);
	if (!ts->pmic)
		return -EPROBE_DEFER;
	if (!device_link_add(&client->dev, &pmic_client->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -ENOMEM;
	usb_device = of_find_device_by_node(ts->usb_node);
	if (!usb_device)
		return -EPROBE_DEFER;
	link = device_link_add(&client->dev, &usb_device->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	ret = link ? 0 : -ENOMEM;
	put_device(&usb_device->dev);
	if (ret)
		return ret;
	atomic_set(&ts->reset_generation, 0);
	atomic_set(&ts->usb_session, 0);
	atomic_set(&ts->computer_seen, 0);
	atomic_set(&ts->usb_events, 0);
	atomic_set(&ts->usb_idle_events, 0);
	/* Allow old iOS work to quiesce; no first-boot ADC idle bit is known. */
	ts->classify_after = jiffies + msecs_to_jiffies(2000);
	ret = devm_mutex_init(&client->dev, &ts->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&ts->work, apple_tristar_poll);
	ret = apple_tristar_update(ts);
	if (ret)
		return dev_err_probe(&client->dev, ret, "CBTL1610 revision/VBUS read failed\n");
	config.drv_data = ts;
	config.fwnode = dev_fwnode(&client->dev);
	ts->psy = devm_power_supply_register(&client->dev, &apple_tristar_desc, &config);
	if (IS_ERR(ts->psy))
		return dev_err_probe(&client->dev, PTR_ERR(ts->psy), "USB supply registration\n");
	ts->usb_notifier.notifier_call = apple_tristar_usb_event;
	ret = usb_gadget_register_current_notifier(&ts->usb_notifier);
	if (ret && ret != -EOPNOTSUPP)
		return ret;
	ts->notifier_registered = !ret;
	ret = devm_add_action_or_reset(&client->dev, apple_tristar_stop, ts);
	if (ret)
		return ret;
	/* Registered after stop, so teardown removes debugfs first. */
	apple_tristar_debugfs_init(ts);
	mod_delayed_work(system_power_efficient_wq, &ts->work, 0);
	dev_info(&client->dev, "CBTL1610 rev 0x8a: guarded Apple source detection, accessory policy %s\n",
		 READ_ONCE(auto_host) ? "on" : "off");
	return 0;
}

static const struct of_device_id apple_tristar_of_match[] = {
	{ .compatible = "apple,j71t-cbtl1610-power" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_tristar_of_match);

static struct i2c_driver apple_tristar_driver = {
	.driver = {
		.name = "apple-tristar-power",
		.of_match_table = apple_tristar_of_match,
		.pm = pm_sleep_ptr(&apple_tristar_pm_ops),
		.dev_groups = apple_tristar_groups,
	},
	.probe = apple_tristar_probe,
	.shutdown = apple_tristar_shutdown,
};
module_i2c_driver(apple_tristar_driver);
MODULE_DESCRIPTION("Apple J71t Tristar source-current detection");
MODULE_LICENSE("GPL");
