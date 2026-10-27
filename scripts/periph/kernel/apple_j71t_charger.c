// SPDX-License-Identifier: GPL-2.0-only
/* Guarded J71t D2365 charging from verified source-current allowances. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "apple_tristar_source.h"

#define J71T_STATUS 0x18a
#define J71T_INPUT_CONTROL 0xa00
#define J71T_INPUT_SUSPEND BIT(1)
#define J71T_INPUT_LIMIT 0xa03
#define J71T_CHARGE_CURRENT 0xa08
#define J71T_MAX_CHARGE_CURRENT 0xa0c
#define J71T_CHARGE_TIMEOUT BIT(4)
#define J71T_CHARGE_END BIT(2)
#define J71T_CHARGE_CODE 50 /* 2500 mA, still bounded by 0xa0c[5:0]. */
#define J71T_USB_CHARGE_CODE 8 /* Conservative 400 mA battery ceiling at 500 mA input. */
#define J71T_REQUEST_UA 2400000
#define J71T_FLOOR_UA 500000
#define J71T_EREGREADBACK (-EUCLEAN)
#define J71T_LINK_GRACE_MS 5000
#define J71T_SAMPLE_GRACE_MS 60000
#define J71T_FAULT_RETRY_MS 60000
#define J71T_OWNER_RETRY_MS 30000
/*
 * Battery sanity window. This unit's cell charges above 4.2 V: iOS reports
 * 4256-4274 mV at 100 % (2026-10-27 capture) and Linux saw 4249 mV at 98 %
 * while charging. With a 4.2 V ceiling the policy stopped charging near full
 * every 30 s. The PMIC owns charge voltage and termination; this window only
 * rejects implausible gauge samples.
 */
#define J71T_VBAT_MIN_UV 3000000
#define J71T_VBAT_MAX_UV 4400000
/* One budget reassert per window before a register change counts as a fault. */
#define J71T_REASSERT_WINDOW_MS 10000
#define J71T_BACKOFF_HOLD_MS 60000
#define J71T_ATTACH_RESET_MS 15000
#define J71T_ATTACH_INPUT_DEFAULT 0x02

static const int j71t_budgets_ua[] = { 500000, 1000000, 2100000, 2400000 };

struct j71t_charger {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
	struct power_supply *source;
	/* Serializes policy changes, register transactions and work lifetime. */
	struct mutex lock;
	struct delayed_work work;
	u8 input_code;
	u8 input_codes[ARRAY_SIZE(j71t_budgets_ua)];
	u8 charge_code;
	int budget_ua;
	int user_limit_ua;
	int max_limit_ua;
	int ceiling_ua;
	int source_type;
	int adaptive_ua;
	unsigned int attach_id;
	unsigned int backoffs;
	unsigned int attach_resets;
	unsigned long attach_until;
	unsigned long adaptive_after;
	const char *backoff_cause;
	bool thermal_limited;
	bool holding;
	bool sample_pending;
	unsigned long hold_until;
	unsigned long sample_until;
	unsigned long fault_until;
	unsigned long retry_after;
	unsigned long ramp_after;
	unsigned long reassert_after;
	u8 old_control;
	u8 old_input;
	u8 old_charge;
	bool calibrated;
	bool saved;
	bool active;
	bool stopping;
	bool disabled;
	bool faulted;
	bool policy_logged;
	int policy_error;
	unsigned long policy_log_after;
};

/* AppleD2365PMU 0x6bc07ec middle/high branches, then 0x6bbff6c. */
static int j71t_calibrate_budget(const u8 *data, size_t size, int target_ma, u8 *code)
{
	s64 point[5], slope, gain, result, ma;
	int i;

	if (!data || size != 24 || target_ma < 500 || target_ma > 2400 ||
	    get_unaligned_le32(data + 20) != 1)
		return -EINVAL;
	for (i = 0; i < 5; i++) {
		point[i] = (s32)get_unaligned_le32(data + i * 4);
		if (point[i] <= 0 || point[i] > (3000LL << 16) ||
		    (i && point[i] <= point[i - 1]))
			return -ERANGE;
	}
	if (target_ma < 1500) {
		slope = div_s64(point[2] - point[1], 53);
		gain = div_s64(point[1] - point[2] + (1025LL << 16), 525);
		result = ((gain * ((target_ma - 475LL) * 65536)) >> 16) -
			 point[1] + (925LL << 16);
	} else {
		slope = div_s64(point[4] - point[3], 30);
		gain = div_s64(point[3] - point[4] + (600LL << 16), 300);
		result = ((gain * ((target_ma - 2100LL) * 65536)) >> 16) -
			 point[3] + (4125LL << 16);
	}
	if (slope < (8LL << 16) || slope > (12LL << 16))
		return -ERANGE;
	ma = result >> 16;
	if (ma < 75 || ma > target_ma)
		return -ERANGE;
	*code = div_s64((ma * 8 - 600) & 0xfff8, 100);
	return *code <= 0xba ? 0 : -ERANGE;
}

static int j71t_calibrate(const u8 *data, size_t size, u8 *code)
{
	return j71t_calibrate_budget(data, size, 2400, code);
}

static int j71t_choose_budget(int source_ua)
{
	int i;

	for (i = ARRAY_SIZE(j71t_budgets_ua) - 1; i >= 0; i--)
		if (source_ua >= j71t_budgets_ua[i])
			return i;
	return -ERANGE;
}

static int j71t_read(struct j71t_charger *charger, unsigned int reg, u8 *value)
{
	unsigned int raw;
	int ret = regmap_read(charger->map, reg, &raw);

	if (!ret)
		*value = raw;
	return ret;
}

static int j71t_write_checked(struct j71t_charger *charger, unsigned int reg, u8 value)
{
	u8 readback;
	int ret;

	ret = regmap_write(charger->map, reg, value);
	if (ret)
		return ret;
	ret = j71t_read(charger, reg, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : J71T_EREGREADBACK;
}

/* Own only bit 1; preserve every other current control-register bit. */
static int j71t_set_suspend(struct j71t_charger *charger, u8 saved)
{
	u8 control;
	int ret;

	ret = j71t_read(charger, J71T_INPUT_CONTROL, &control);
	if (ret)
		return ret;
	if ((control & J71T_INPUT_SUSPEND) == (saved & J71T_INPUT_SUSPEND))
		return 0;
	control = (control & ~J71T_INPUT_SUSPEND) | (saved & J71T_INPUT_SUSPEND);
	return j71t_write_checked(charger, J71T_INPUT_CONTROL, control);
}

static int j71t_online(struct j71t_charger *charger)
{
	union power_supply_propval value;
	int ret;

	if (!charger->psy)
		return -EAGAIN;
	ret = power_supply_get_property_from_supplier(charger->psy,
						      POWER_SUPPLY_PROP_ONLINE, &value);
	return ret ? ret : !!value.intval;
}

static int j71t_source_limit(struct j71t_charger *charger)
{
	union power_supply_propval value;
	int ret;

	if (!charger->psy)
		return -EAGAIN;
	ret = power_supply_get_property_from_supplier(charger->psy,
						      POWER_SUPPLY_PROP_CURRENT_MAX, &value);
	return ret ? ret : value.intval;
}

/* An unknown DT ceiling must never grant an uncalibrated budget. */
static int j71t_valid_limit(unsigned int limit)
{
	int index = j71t_choose_budget(limit);

	return index >= 0 && (unsigned int)j71t_budgets_ua[index] == limit ? limit : J71T_FLOOR_UA;
}

static int j71t_source_eligible(struct j71t_charger *charger)
{
	union power_supply_propval value;
	int ret;

	ret = power_supply_get_property_from_supplier(charger->psy,
						      POWER_SUPPLY_PROP_USB_TYPE, &value);
	if (ret)
		return -EAGAIN;
	/* ACA: the identified camera adapter, a source while the iPad is the host. */
	return value.intval == POWER_SUPPLY_USB_TYPE_SDP ||
	       value.intval == POWER_SUPPLY_USB_TYPE_CDP ||
	       value.intval == POWER_SUPPLY_USB_TYPE_DCP ||
	       value.intval == POWER_SUPPLY_USB_TYPE_C ||
	       value.intval == POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID ||
	       value.intval == POWER_SUPPLY_USB_TYPE_ACA;
}

/* Link PM/reset is not source removal; only ONLINE=0 revokes the owner. */
static int j71t_policy_limit(struct j71t_charger *charger)
{
	int source_ua = j71t_source_limit(charger);
	union power_supply_propval type;
	int eligible;

	if (source_ua < 0)
		return -EAGAIN;
	eligible = j71t_source_eligible(charger);
	if (eligible < 0)
		return eligible;
	if (power_supply_get_property_from_supplier(charger->psy,
						    POWER_SUPPLY_PROP_USB_TYPE, &type))
		return -EAGAIN;
	charger->source_type = type.intval;
	/* iOS USBHost policy, separate from the USB stack's enumeration budget. */
	if (type.intval == POWER_SUPPLY_USB_TYPE_SDP)
		source_ua = charger->max_limit_ua;
	/* A reset can temporarily lose classification without losing VBUS. */
	if (source_ua < J71T_FLOOR_UA || !eligible) {
		charger->ceiling_ua = J71T_FLOOR_UA;
		if (!charger->holding) {
			charger->holding = true;
			charger->hold_until = jiffies + msecs_to_jiffies(J71T_LINK_GRACE_MS);
		}
		if (charger->active && time_before(jiffies, charger->hold_until))
			return charger->budget_ua;
		return J71T_FLOOR_UA;
	}
	charger->holding = false;
	charger->ceiling_ua = min(charger->max_limit_ua, source_ua);
	source_ua = charger->ceiling_ua;
	if (charger->user_limit_ua)
		source_ua = min(source_ua, charger->user_limit_ua);
	if (charger->adaptive_ua)
		source_ua = min(source_ua, charger->adaptive_ua);
	return source_ua;
}

static int j71t_charge_code(int budget_ua)
{
	return budget_ua == 500000 ? J71T_USB_CHARGE_CODE : J71T_CHARGE_CODE;
}

static void j71t_revoke_override(struct j71t_charger *charger)
{
	if (charger->user_limit_ua)
		dev_info(charger->dev, "owner input override revoked\n");
	charger->user_limit_ua = 0;
}

/* Called even when an offline interval fell entirely between policy ticks. */
static void j71t_new_attach(struct j71t_charger *charger, unsigned int id)
{
	j71t_revoke_override(charger);
	charger->disabled = false;
	charger->holding = false;
	charger->sample_pending = false;
	charger->hold_until = 0;
	charger->sample_until = 0;
	charger->retry_after = 0;
	charger->ramp_after = 0;
	charger->reassert_after = jiffies;
	charger->adaptive_ua = 0;
	charger->adaptive_after = 0;
	charger->backoff_cause = NULL;
	charger->backoffs = 0;
	charger->ceiling_ua = 0;
	charger->source_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	charger->attach_id = id;
	charger->attach_resets = 0;
	charger->attach_until = jiffies + msecs_to_jiffies(J71T_ATTACH_RESET_MS);
}

static int j71t_battery_safe(struct j71t_charger *charger)
{
	struct power_supply *battery;
	union power_supply_propval value;
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery)
		return -EAGAIN;
	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_PRESENT, &value);
	if (ret)
		goto transient;
	/* PRESENT=0 also means a failed cached flags read in bq27xxx. */
	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_NOW, &value);
	if (ret)
		goto transient;
	if (value.intval < J71T_VBAT_MIN_UV || value.intval > J71T_VBAT_MAX_UV) {
		ret = -ERANGE;
		goto out;
	}
	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_TEMP, &value);
	if (ret)
		goto transient;
	if (value.intval >= 450)
		charger->thermal_limited = true;
	else if (value.intval <= 420)
		charger->thermal_limited = false;
	ret = value.intval < 100 ? -ERANGE :
		charger->thermal_limited ? -EOVERFLOW : 0;
	goto out;
transient:
	ret = -EAGAIN;
out:
	power_supply_put(battery);
	return ret;
}

/* Match the iOS order without using input suspend as a transaction lock. */
static int j71t_apply(struct j71t_charger *charger, u8 charge, u8 input)
{
	int ret = j71t_write_checked(charger, J71T_CHARGE_CURRENT, charge);

	if (!ret)
		ret = j71t_write_checked(charger, J71T_INPUT_LIMIT, input);
	return ret;
}

static int j71t_restore(struct j71t_charger *charger)
{
	int ret;

	charger->active = false;
	if (!charger->saved)
		return 0;
	ret = j71t_apply(charger, charger->old_charge, charger->old_input);
	if (!ret)
		charger->saved = false;
	return ret;
}

static bool j71t_is_fault(int error)
{
	return error == J71T_EREGREADBACK;
}

static void j71t_fault(struct j71t_charger *charger, int error)
{
	if (j71t_is_fault(error)) {
		charger->faulted = true;
		charger->fault_until = jiffies + msecs_to_jiffies(J71T_FAULT_RETRY_MS);
	}
}

static int j71t_guard(struct j71t_charger *charger)
{
	u8 status, cap;
	unsigned int id;
	int ret = j71t_online(charger);

	if (ret != 1)
		return ret < 0 ? ret : -ENOLINK;
	ret = apple_tristar_attach_id(charger->source, &id);
	if (ret || id != charger->attach_id)
		return ret ?: -ENOLINK;
	ret = j71t_policy_limit(charger);
	if (ret < charger->budget_ua)
		return ret < 0 ? ret : -ERANGE;
	ret = j71t_battery_safe(charger);
	if (ret == -EOVERFLOW && charger->budget_ua == J71T_FLOOR_UA)
		ret = 0;
	if (ret)
		return ret;
	ret = j71t_read(charger, J71T_STATUS, &status);
	if (ret)
		return ret;
	ret = j71t_read(charger, J71T_MAX_CHARGE_CURRENT, &cap);
	if (ret)
		return ret;
	if ((cap & 0x3f) < charger->charge_code)
		return -ERANGE;
	if (status & J71T_CHARGE_TIMEOUT)
		return -ETIME;
	return status & J71T_CHARGE_END ? -EBUSY : 0;
}

static int j71t_enable(struct j71t_charger *charger)
{
	int ret;

	if (!charger->calibrated)
		return -ENODATA;
	ret = j71t_guard(charger);
	if (charger->budget_ua == J71T_FLOOR_UA && (ret == -ETIME || ret == -EBUSY))
		ret = 0;
	if (ret)
		return ret;
	if (!charger->saved) {
		ret = j71t_read(charger, J71T_INPUT_CONTROL, &charger->old_control);
		if (!ret)
			ret = j71t_read(charger, J71T_INPUT_LIMIT, &charger->old_input);
		if (!ret)
			ret = j71t_read(charger, J71T_CHARGE_CURRENT, &charger->old_charge);
		if (ret)
			return ret;
		charger->saved = true;
		charger->attach_until = jiffies + msecs_to_jiffies(J71T_ATTACH_RESET_MS);
	}
	ret = j71t_apply(charger, charger->charge_code, charger->input_code);
	if (!ret) {
		ret = j71t_guard(charger);
		if (charger->budget_ua == J71T_FLOOR_UA && (ret == -ETIME || ret == -EBUSY))
			ret = 0;
	}
	/* Clear a genuine handoff/owner disable only after the budgets are valid. */
	if (!ret && (charger->old_control & J71T_INPUT_SUSPEND)) {
		ret = j71t_set_suspend(charger, 0);
		if (!ret)
			charger->old_control &= ~J71T_INPUT_SUSPEND;
	}
	if (ret) {
		int restore = j71t_restore(charger);

		j71t_fault(charger, ret);
		j71t_fault(charger, restore);
		return ret;
	}
	charger->active = true;
	charger->faulted = false;
	charger->fault_until = 0;
	dev_info_ratelimited(charger->dev,
			     "%s budget %d mA: input 0x%02x, battery limit %d mA, type %d ceiling %d mA, back-off %s\n",
			     charger->user_limit_ua ? "owner override" : "source",
			     charger->budget_ua / 1000, charger->input_code,
			     charger->charge_code * 50,
			     charger->source_type, charger->ceiling_ua / 1000,
			     charger->backoff_cause ?: "none");
	return 0;
}

/* Log transitions immediately and persistent deferred decisions every 30 s. */
static void j71t_policy_log(struct j71t_charger *charger, int error)
{
	if (charger->policy_logged && charger->policy_error == error &&
	    time_before(jiffies, charger->policy_log_after))
		return;
	charger->policy_logged = true;
	charger->policy_error = error;
	charger->policy_log_after = jiffies + msecs_to_jiffies(30000);
	if (charger->faulted)
		dev_info(charger->dev, "charging policy ownership fault; automatic retry in 60 s\n");
	else if (charger->disabled)
		dev_info(charger->dev, "charging policy disabled by owner\n");
	else if (!charger->calibrated)
		dev_info(charger->dev, "charging policy blocked: valid CBAT required\n");
	else if (error == -EAGAIN)
		dev_info(charger->dev, "charging policy waiting for valid battery/source sample; retry in 1000 ms\n");
	else if (error)
		dev_info(charger->dev, "charging policy deferred: %d; retry scheduled\n", error);
}

static int j71t_check_owned(struct j71t_charger *charger, u8 *input, u8 *control, u8 *charge)
{
	int ret = j71t_read(charger, J71T_INPUT_LIMIT, input);

	if (!ret)
		ret = j71t_read(charger, J71T_INPUT_CONTROL, control);
	if (!ret)
		ret = j71t_read(charger, J71T_CHARGE_CURRENT, charge);
	if (!ret && (*input != charger->input_code ||
		     (*control & J71T_INPUT_SUSPEND) || *charge != charger->charge_code))
		ret = J71T_EREGREADBACK;
	return ret;
}

/*
 * The PMIC changes the input settings on its own shortly after a source
 * attaches (seen 0.15 s and about 1 s after the first enable, 2026-10-26/27).
 * Put the active budget back once per window; a second change within the
 * window is a real ownership fault.
 */
static int j71t_reassert(struct j71t_charger *charger, u8 input, u8 control, u8 charge)
{
	int ret;

	if (time_before(jiffies, charger->reassert_after))
		return J71T_EREGREADBACK;
	charger->reassert_after = jiffies + msecs_to_jiffies(J71T_REASSERT_WINDOW_MS);
	dev_info(charger->dev,
		 "charger registers changed (input 0x%02x, control 0x%02x, charge 0x%02x); reasserting the %d mA budget\n",
		 input, control, charge, charger->budget_ua / 1000);
	ret = j71t_apply(charger, charger->charge_code, charger->input_code);
	if (!ret && (control & J71T_INPUT_SUSPEND))
		ret = j71t_set_suspend(charger, 0);
	if (!ret)
		ret = j71t_check_owned(charger, &input, &control, &charge);
	return ret;
}

/*
 * Reduce the policy step after a non-default lower PMU setting; then assert
 * the calibrated code for that step. Its electrical cause is unknown:
 * D2365's iOS isInputLimited callback (6bc038c) is constant false. This is
 * an observed setpoint reduction, not a fabricated VBUS/droop measurement.
 */
static int j71t_input_reduced(struct j71t_charger *charger, u8 input)
{
	int index = j71t_choose_budget(charger->budget_ua);
	int ret;

	if (index > 0)
		index--;
	while (index > 0 && charger->input_codes[index] > input)
		index--;
	charger->adaptive_ua = j71t_budgets_ua[index];
	charger->adaptive_after = jiffies + msecs_to_jiffies(J71T_BACKOFF_HOLD_MS);
	charger->backoff_cause = "pmic-input-reduced";
	charger->backoffs++;
	charger->budget_ua = charger->adaptive_ua;
	charger->input_code = charger->input_codes[index];
	charger->charge_code = j71t_charge_code(charger->budget_ua);
	ret = j71t_enable(charger);
	dev_warn_ratelimited(charger->dev,
			     "PMU input reduced to 0x%02x; back-off %d mA for 60 s (cause unverified), result %d\n",
			     input, charger->budget_ua / 1000, ret);
	return ret;
}

/* A readable hot pack must still trip while another supplier read fails. */
static int j71t_thermal_floor(struct j71t_charger *charger)
{
	u8 cap;
	int ret = j71t_read(charger, J71T_MAX_CHARGE_CURRENT, &cap);

	if (ret)
		return ret;
	if ((cap & 0x3f) < J71T_USB_CHARGE_CODE)
		return -ERANGE;
	ret = j71t_apply(charger, J71T_USB_CHARGE_CODE, charger->input_codes[0]);
	if (!ret) {
		charger->budget_ua = J71T_FLOOR_UA;
		charger->charge_code = J71T_USB_CHARGE_CODE;
		charger->input_code = charger->input_codes[0];
		charger->retry_after = jiffies + msecs_to_jiffies(J71T_OWNER_RETRY_MS);
		dev_warn(charger->dev, "thermal floor while another safety sample is unavailable\n");
	}
	return ret;
}

static void j71t_policy_work(struct work_struct *work)
{
	struct j71t_charger *charger = container_of(to_delayed_work(work),
							  struct j71t_charger, work);
	int ret = 0, target, index, online, previous;
	unsigned int id;
	bool attach_reset = false;

	mutex_lock(&charger->lock);
	if (charger->stopping)
		goto out;
	if (charger->faulted && !time_before(jiffies, charger->fault_until))
		charger->faulted = false;
	online = j71t_online(charger);
	ret = apple_tristar_attach_id(charger->source, &id);
	if (!ret && id != charger->attach_id) {
		j71t_new_attach(charger, id);
		ret = j71t_restore(charger);
		if (ret)
			goto report;
	}
	if (!online) {
		charger->holding = false;
		charger->sample_pending = false;
		ret = j71t_restore(charger);
		if (!ret)
			ret = j71t_set_suspend(charger, J71T_INPUT_SUSPEND);
		goto report;
	}
	if (charger->disabled || charger->faulted || !charger->calibrated) {
		ret = j71t_restore(charger);
		goto report;
	}
	/* A failed rollback must finish before taking a fresh snapshot. */
	if (charger->saved && !charger->active) {
		ret = j71t_restore(charger);
		if (ret)
			goto report;
	}
	if (charger->active) {
		u8 input = 0, control = 0, charge = 0;

		ret = j71t_check_owned(charger, &input, &control, &charge);
		if (j71t_is_fault(ret) && input == J71T_ATTACH_INPUT_DEFAULT &&
		    time_before(jiffies, charger->attach_until) &&
		    !(control & J71T_INPUT_SUSPEND) && charge == charger->charge_code) {
			/* D2365 attach reset, observed 26 ms after applying the floor. */
			attach_reset = true;
			ret = 0;
		}
		if (j71t_is_fault(ret) && input < charger->input_code &&
		    !(control & J71T_INPUT_SUSPEND) && charge == charger->charge_code) {
			ret = j71t_input_reduced(charger, input);
			if (ret)
				goto restore;
		}
		if (j71t_is_fault(ret))
			ret = j71t_reassert(charger, input, control, charge);
		if (j71t_is_fault(ret))
			goto restore;
		if (ret)
			goto transient;
	}
	if (online < 0)
		goto transient;
	/* One slow upward retry per minute after an observed PMU reduction. */
	if (charger->adaptive_ua && !time_before(jiffies, charger->adaptive_after)) {
		index = j71t_choose_budget(charger->adaptive_ua);
		if (index + 1 < (int)ARRAY_SIZE(j71t_budgets_ua))
			charger->adaptive_ua = j71t_budgets_ua[index + 1];
		charger->adaptive_after = jiffies + msecs_to_jiffies(J71T_BACKOFF_HOLD_MS);
	}
	target = j71t_policy_limit(charger);
	if (target < 0)
		goto transient;
	ret = j71t_battery_safe(charger);
	if (ret == -EAGAIN)
		goto transient;
	if (ret && ret != -EOVERFLOW)
		goto restore;
	if (ret == -EOVERFLOW)
		target = J71T_FLOOR_UA;
	/* Guard the existing budget before considering a new one. */
	if (charger->active) {
		ret = j71t_guard(charger);
		if (ret == -EOVERFLOW || ret == -ETIME || ret == -EBUSY)
			target = J71T_FLOOR_UA;
		else if (ret == -ERANGE && target < charger->budget_ua)
			ret = 0;
		else if (ret == -EAGAIN || (ret && ret != -ERANGE && ret != -ENOLINK))
			goto transient;
		else if (ret)
			goto restore;
	}
	charger->sample_pending = false;
	previous = charger->active ? charger->budget_ua : 0;
	if (target < previous)
		charger->retry_after = jiffies + msecs_to_jiffies(J71T_OWNER_RETRY_MS);
	else if (previous && target > previous &&
		 time_before(jiffies, charger->retry_after))
		target = previous;
	index = j71t_choose_budget(target);
	/* At most one upward step per second, including supplier notifications. */
	if (index >= 0 && target > previous) {
		int next = previous ? j71t_choose_budget(previous) + 1 : 0;

		if (previous && time_before(jiffies, charger->ramp_after)) {
			if (!attach_reset)
				goto report;
			next = j71t_choose_budget(previous);
		}
		index = min(index, next);
	}
	if (index >= 0 &&
	    (!charger->active || j71t_budgets_ua[index] != previous || attach_reset)) {
		charger->budget_ua = j71t_budgets_ua[index];
		charger->input_code = charger->input_codes[index];
		charger->charge_code = j71t_charge_code(charger->budget_ua);
		ret = j71t_enable(charger);
		if (ret)
			goto restore;
		if (attach_reset) {
			charger->attach_resets++;
			dev_info_ratelimited(charger->dev,
					     "attach default 0x02: restored policy input 0x%02x\n",
					     charger->input_code);
		}
		charger->ramp_after = jiffies + msecs_to_jiffies(1000);
	}
	goto report;
transient:
	/* Source/PMIC transport grace must not mask a valid pack guard. */
	ret = j71t_battery_safe(charger);
	if (ret == -ERANGE)
		goto restore;
	if (ret == -EOVERFLOW && charger->active && charger->budget_ua > J71T_FLOOR_UA) {
		ret = j71t_thermal_floor(charger);
		if (ret)
			goto restore;
	}
	ret = -EAGAIN;
	if (!charger->sample_pending) {
		charger->sample_pending = true;
		charger->sample_until = jiffies + msecs_to_jiffies(J71T_SAMPLE_GRACE_MS);
	}
	if (!charger->active || time_before(jiffies, charger->sample_until))
		goto report;
	dev_warn(charger->dev, "source/battery sample unavailable for 60 s; restoring handoff\n");
restore:
	j71t_fault(charger, ret);
	if (charger->active)
		dev_warn(charger->dev, "charging policy stepped down: %d\n", ret);
	j71t_fault(charger, j71t_restore(charger));
	charger->retry_after = jiffies + msecs_to_jiffies(J71T_OWNER_RETRY_MS);
report:
	j71t_fault(charger, ret);
	if (charger->faulted)
		ret = J71T_EREGREADBACK;
	else if (charger->disabled)
		ret = -ECANCELED;
	else if (!charger->calibrated)
		ret = -ENODATA;
	j71t_policy_log(charger, ret);
	mod_delayed_work(system_power_efficient_wq, &charger->work,
			 msecs_to_jiffies(1000));
out:
	mutex_unlock(&charger->lock);
	if (charger->psy)
		power_supply_changed(charger->psy);
}

static int j71t_get_property(struct power_supply *psy, enum power_supply_property prop,
			     union power_supply_propval *value)
{
	struct j71t_charger *charger = power_supply_get_drvdata(psy);
	u8 raw;
	int ret, online;

	mutex_lock(&charger->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		online = j71t_online(charger);
		ret = online < 0 ? online : 0;
		value->intval = online == 1;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		ret = power_supply_get_property_from_supplier(psy,
							      POWER_SUPPLY_PROP_USB_TYPE, value);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		value->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = j71t_read(charger, J71T_INPUT_LIMIT, &raw);
		if (!ret)
			value->intval = 75000 + raw * 12500;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = j71t_read(charger, J71T_CHARGE_CURRENT, &raw);
		if (!ret)
			value->intval = raw * 50000;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = j71t_read(charger, J71T_STATUS, &raw);
		online = j71t_online(charger);
		if (!ret) {
			value->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			if (online == 1 && !(raw & (J71T_CHARGE_TIMEOUT | J71T_CHARGE_END)) &&
			    (raw & 3))
				value->intval = POWER_SUPPLY_STATUS_CHARGING;
		}
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&charger->lock);
	return ret;
}

static int j71t_set_property(struct power_supply *psy, enum power_supply_property prop,
			     const union power_supply_propval *value)
{
	struct j71t_charger *charger = power_supply_get_drvdata(psy);
	int ret = 0;

	if (prop != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT || value->intval < 0 ||
	    (value->intval && j71t_valid_limit(value->intval) != value->intval))
		return -EINVAL;
	mutex_lock(&charger->lock);
	if (charger->stopping) {
		ret = -ESHUTDOWN;
	} else if (value->intval > charger->max_limit_ua) {
		ret = -EINVAL;
	} else if (!value->intval) {
		charger->disabled = true;
		j71t_revoke_override(charger);
		ret = j71t_restore(charger);
		/* Target zero is a real disable, never a budget-change ritual. */
		if (!ret)
			ret = j71t_set_suspend(charger, J71T_INPUT_SUSPEND);
	} else {
		charger->disabled = false;
		charger->user_limit_ua = value->intval;
		/* Authorization persists across deferred guards; work applies it. */
		dev_info(charger->dev, "owner authorizes %d mA input\n", value->intval / 1000);
	}
	j71t_fault(charger, ret);
	if (!charger->stopping)
		mod_delayed_work(system_power_efficient_wq, &charger->work, 0);
	mutex_unlock(&charger->lock);
	power_supply_changed(psy);
	return ret;
}

static int j71t_property_writeable(struct power_supply *psy, enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static void j71t_external_power_changed(struct power_supply *psy)
{
	struct j71t_charger *charger = power_supply_get_drvdata(psy);

	mutex_lock(&charger->lock);
	if (!charger->stopping)
		mod_delayed_work(system_power_efficient_wq, &charger->work, 0);
	mutex_unlock(&charger->lock);
}

static const enum power_supply_property j71t_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
};

static const struct power_supply_desc j71t_desc = {
	.name = "charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID) | BIT(POWER_SUPPLY_USB_TYPE_ACA) |
		     BIT(POWER_SUPPLY_USB_TYPE_C),
	.properties = j71t_properties,
	.num_properties = ARRAY_SIZE(j71t_properties),
	.get_property = j71t_get_property,
	.set_property = j71t_set_property,
	.property_is_writeable = j71t_property_writeable,
	.external_power_changed = j71t_external_power_changed,
};

static ssize_t charge7_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct j71t_charger *charger = power_supply_get_drvdata(dev_get_drvdata(dev));
	u8 input = 0;
	int ret, len;

	mutex_lock(&charger->lock);
	ret = j71t_read(charger, J71T_INPUT_LIMIT, &input);
	len = sysfs_emit(buf,
			 "attach=%u source_type=%d ceiling_ua=%d step_ua=%d live_ua=%d override_ua=%d adaptive_ua=%d backoffs=%u last_backoff=%s vbus_mv=unavailable policy_error=%d active=%d attach_resets=%u\n",
			 charger->attach_id, charger->source_type, charger->ceiling_ua,
			 charger->active ? charger->budget_ua : 0,
			 ret ? ret : 75000 + input * 12500, charger->user_limit_ua,
			 charger->adaptive_ua, charger->backoffs,
			 charger->backoff_cause ?: "none", charger->policy_error, charger->active,
			 charger->attach_resets);
	mutex_unlock(&charger->lock);
	return len;
}
static DEVICE_ATTR_RO(charge7_state);

static struct attribute *j71t_attrs[] = {
	&dev_attr_charge7_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(j71t);

static int j71t_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	const struct firmware *fw;
	struct j71t_charger *charger;
	u32 limit;
	int ret, i;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;
	charger->dev = &pdev->dev;
	charger->source = devm_power_supply_get_by_reference(&pdev->dev, "power-supplies");
	if (IS_ERR(charger->source))
		return PTR_ERR(charger->source);
	if (!charger->source)
		return -EPROBE_DEFER;
	if (!device_link_add(&pdev->dev, &charger->source->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -ENOMEM;
	charger->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!charger->map)
		return -ENODEV;
	ret = devm_mutex_init(&pdev->dev, &charger->lock);
	if (ret)
		return ret;
	ret = devm_delayed_work_autocancel(&pdev->dev, &charger->work, j71t_policy_work);
	if (ret)
		return ret;
	ret = request_firmware_direct(&fw, "apple/j71t-CBAT.bin", &pdev->dev);
	if (!ret) {
		for (i = 0; i < ARRAY_SIZE(j71t_budgets_ua); i++) {
			if (j71t_budgets_ua[i] == J71T_REQUEST_UA)
				ret = j71t_calibrate(fw->data, fw->size, &charger->input_codes[i]);
			else
				ret = j71t_calibrate_budget(fw->data, fw->size,
							    j71t_budgets_ua[i] / 1000,
							    &charger->input_codes[i]);
			if (ret)
				break;
		}
		release_firmware(fw);
		charger->calibrated = !ret;
	}
	if (ret)
		dev_warn(&pdev->dev,
			 "CBAT unavailable/unsupported (%d): charging writes disabled\n", ret);
	charger->reassert_after = jiffies;
	charger->max_limit_ua = J71T_FLOOR_UA;
	if (!of_property_read_u32(pdev->dev.of_node,
				  "apple,usb-input-limit-max-microamp", &limit))
		charger->max_limit_ua = j71t_valid_limit(limit);
	config.drv_data = charger;
	config.fwnode = dev_fwnode(&pdev->dev);
	config.attr_grp = j71t_groups;
	charger->psy = devm_power_supply_register(&pdev->dev, &j71t_desc, &config);
	if (IS_ERR(charger->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(charger->psy),
				     "charger supply registration\n");
	platform_set_drvdata(pdev, charger);
	mod_delayed_work(system_power_efficient_wq, &charger->work, 0);
	dev_info(&pdev->dev, "J71t charger: automatic guarded source-current policy\n");
	return 0;
}

static int j71t_quiesce(struct device *dev)
{
	struct j71t_charger *charger = dev_get_drvdata(dev);
	int attempt, ret = 0;

	mutex_lock(&charger->lock);
	charger->stopping = true;
	mutex_unlock(&charger->lock);
	cancel_delayed_work_sync(&charger->work);
	for (attempt = 0; attempt < 3; attempt++) {
		mutex_lock(&charger->lock);
		ret = j71t_restore(charger);
		mutex_unlock(&charger->lock);
		if (!ret)
			break;
		msleep(20);
	}
	if (ret)
		dev_crit(charger->dev, "charger restore failed at shutdown/remove: %d\n", ret);
	return ret;
}

static void j71t_stop(struct platform_device *pdev)
{
	j71t_quiesce(&pdev->dev);
}

static int j71t_resume(struct device *dev)
{
	struct j71t_charger *charger = dev_get_drvdata(dev);

	mutex_lock(&charger->lock);
	charger->stopping = false;
	mod_delayed_work(system_power_efficient_wq, &charger->work, 0);
	mutex_unlock(&charger->lock);
	return 0;
}

static int j71t_suspend(struct device *dev)
{
	int ret = j71t_quiesce(dev);

	/* A failed suspend need not receive resume; keep rollback retries alive. */
	if (ret)
		j71t_resume(dev);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(j71t_pm_ops, j71t_suspend, j71t_resume);

static const struct of_device_id j71t_match[] = {
	{ .compatible = "apple,j71t-d2365-charger" },
	{}
};
MODULE_DEVICE_TABLE(of, j71t_match);

static struct platform_driver j71t_driver = {
	.probe = j71t_probe,
	.remove = j71t_stop,
	.shutdown = j71t_stop,
	.driver = {
		.name = "apple-j71t-charger",
		.of_match_table = j71t_match,
		.pm = pm_sleep_ptr(&j71t_pm_ops),
	},
};
module_platform_driver(j71t_driver);
MODULE_DESCRIPTION("Source-aware guarded Apple J71t D2365 charging policy");
MODULE_LICENSE("GPL");
