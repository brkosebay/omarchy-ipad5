/* SPDX-License-Identifier: GPL-2.0-only */
/* Private apple-soc-cpufreq implementation; included after the request helper. */
static unsigned int a9_psinfo1_offset(unsigned int state)
{
	return state < 8 ? 0x68 + state * 8 : 0x368 + (state - 8) * 8;
}

static unsigned int a9_psinfo_khz(u64 word)
{
	unsigned int prediv = FIELD_GET(GENMASK_ULL(17, 13), word);

	/* Matching j71t iBoot 20H350 at 0x87001c35c. Zero prediv means one. */
	return 24000 * FIELD_GET(GENMASK_ULL(12, 4), word) /
		max(prediv, 1U) / (FIELD_GET(GENMASK_ULL(3, 0), word) + 1);
}

#ifdef CONFIG_APPLE_A9_TUNING
#define A9_STOCK_KHZ 1800000U
#define A9_BOOST_KHZ 1848000U
#define A9_PLL_MAX_KHZ (511U * 24000U) /* PSINFO1[12:4], stock dividers. */
#define A9_BOOST_STATE 15U /* All-load stock clone; native slot 8 stays intact. */
#define A9_OVERVOLT_LIMIT_UV 50000U /* Owner ceiling: never a module parameter. */
#define A9_DAC_STEP_UV 3125U
#define A9_CORE_BASE_UV 450000U
#define A9_AUX_BASE_UV 600000U
#define A9_HEARTBEAT_MS 2000
#define A9_POLL_MS 100

static bool a9_tuning;
module_param(a9_tuning, bool, 0400);
MODULE_PARM_DESC(a9_tuning, "Enable guarded RAM-only S8003 tuning (default off)");

/*
 * Hottest trusted sensor at which the guard reverts to stock (passive).
 * 80 C by default; owner-selected up to 92 C. The hot (95 C) and critical
 * (100 C) reverts and the hardware thermal protection are unchanged.
 */
static unsigned int a9_passive_millic = 80000;
module_param(a9_passive_millic, uint, 0400);
MODULE_PARM_DESC(a9_passive_millic, "Guard passive revert threshold in milli-C, 70000..92000 (default 80000)");

/* Boot parameters are parsed before either driver's probe. No guard state or
 * probe ordering is involved, and the parameters are read-only after boot.
 * One selection controls the guard and the thermal framework's cooling trips.
 */
struct apple_a9_thermal_profile apple_a9_get_thermal_profile(void)
{
	struct apple_a9_thermal_profile p = apple_a9_stock_thermal_profile();

	if (a9_tuning && of_machine_is_compatible("apple,j71t")) {
		p.tuning = true;
		p.passive = clamp(a9_passive_millic, 70000U, 92000U);
		p.hot = 95000;
		p.arm = p.passive - 5000;
		p.cooldown = p.passive - 10000;
	}
	return p;
}
EXPORT_SYMBOL_GPL(apple_a9_get_thermal_profile);

static const unsigned int a9_step_khz[] = {
	1896000, 1944000, 1992000, 2040000, 2088000, 2160000,
};

/* Preserve the round-5 mapping; higher OPPs share the last dormant slot.
 * 48 MHz rungs end at 12240 MHz, then the final 24 MHz reaches multiplier 511.
 * This is an encoding bound, not an analog PLL operating-range claim.
 */
static unsigned int a9_step_state(unsigned int khz)
{
	unsigned int i;

	if (khz == A9_BOOST_KHZ)
		return A9_BOOST_STATE;
	for (i = 0; i < ARRAY_SIZE(a9_step_khz); i++)
		if (khz == a9_step_khz[i])
			return i + 9;
	if ((khz > 2160000 && khz <= A9_PLL_MAX_KHZ &&
	     (khz - 2160000) % 48000 == 0) || khz == A9_PLL_MAX_KHZ)
		return 14;
	return 0;
}

static const char * const a9_zone_names[] = {
	"apple-a9-soc0", "apple-a9-soc1", "apple-a9-tcc2",
};

struct a9_guard {
	struct cpufreq_policy *policy;
	struct apple_cpu_priv *priv;
	struct delayed_work work;
	struct mutex control;
	struct freq_qos_request qos;
	struct dentry *directory;
	void __iomem *voltage_base;
	u64 stock1[16], stock2[16];
	unsigned int limit, configured, dirty, delta_uv;
	u32 trip_generation;
	u32 voltage_ctl_boot, voltage_routes_boot;
	int control_error;
	unsigned long heartbeat;
	bool armed, ready, latched;
	bool thermal_qos_pending;
	const char *reason;
	int revert_error, hottest;
};

static struct a9_guard __rcu *a9_active_guard;

/* The cold-boot j71t logs observe 0/1782, not a sticky copy of m1n1's
 * write of 1. This is an exact observed-handoff allowlist, not a new hardware
 * mode decoder. Preserve it; never program voltage control, DWI or the PMIC.
 * Both the initial handoff and every live check must match. Unknown bits,
 * routing changes and a changed handoff are refused even at delta zero.
 */
static bool a9_voltage_handoff_ok(struct a9_guard *g)
{
	return g->voltage_ctl_boot == 0 && g->voltage_routes_boot == 0x1782 &&
		readl(g->voltage_base) == 0 && readl(g->voltage_base + 4) == 0x1782;
}

/* LLB 0x180396138 converts logical rails 0/7 through the PMIC table before
 * placing them in PSINFO1[63:56]/PSINFO2[39:32]. iBoot independently exports
 * core mV from that byte using base 450 and step 3125 uV. Buck7 has base 600.
 * Apply equal exact DAC increments to both rails; retain the boot separation.
 * See docs/power/overclock-round3.md in the workspace for hashes and evidence.
 */
static int a9_voltage_words(u64 stock1, u64 stock2, unsigned int delta_uv,
			    u64 *word1, u64 *word2)
{
	unsigned int core = FIELD_GET(GENMASK_ULL(63, 56), stock1);
	unsigned int aux = FIELD_GET(GENMASK_ULL(39, 32), stock2);
	unsigned int steps;

	if (delta_uv > A9_OVERVOLT_LIMIT_UV)
		return -ERANGE;
	/* Refuse fractional codes, including rounding that could exceed +50 mV. */
	if (delta_uv % A9_DAC_STEP_UV)
		return -EINVAL;
	steps = delta_uv / A9_DAC_STEP_UV;
	if (core + steps > 255 || aux + steps > 255)
		return -ERANGE;
	if (A9_CORE_BASE_UV + (core + steps) * A9_DAC_STEP_UV > 1296000 ||
	    A9_AUX_BASE_UV + (aux + steps) * A9_DAC_STEP_UV > 1396000)
		return -ERANGE;
	*word1 = (stock1 & ~GENMASK_ULL(63, 56)) |
		FIELD_PREP(GENMASK_ULL(63, 56), core + steps);
	*word2 = (stock2 & ~GENMASK_ULL(39, 32)) |
		FIELD_PREP(GENMASK_ULL(39, 32), aux + steps);
	return 0;
}

static int a9_temperatures(int *hottest)
{
	int i, temp, ret;
	struct thermal_zone_device *zone;

	*hottest = INT_MIN;
	for (i = 0; i < ARRAY_SIZE(a9_zone_names); i++) {
		zone = thermal_zone_get_zone_by_name(a9_zone_names[i]);
		if (IS_ERR(zone))
			return PTR_ERR(zone);
		ret = thermal_zone_get_temp(zone, &temp);
		if (ret)
			return ret;
		if (temp < -40000 || temp > 150000)
			return -ERANGE;
		*hottest = max(*hottest, temp);
	}
	return 0;
}

/* Called under priv->lock: blocks every target, including racing governors. */
static int a9_restore_locked(struct a9_guard *g)
{
	struct apple_cpu_priv *priv = g->priv;
	u64 command;
	u32 status;
	unsigned int fallback;
	int ret, state;

	g->limit = A9_STOCK_KHZ;
	status = readl(priv->reg_base + APPLE_DVFS_STATUS);
	fallback = (status >> 4) & 15;
	if (!fallback || fallback > 7)
		fallback = 7;
	/* Never raise a CPU already cooled below the stock maximum. */
	ret = apple_soc_cpufreq_request(priv, fallback);
	if (ret)
		return ret;
	status = readl(priv->reg_base + APPLE_DVFS_STATUS);
	command = readq(priv->reg_base + APPLE_DVFS_CMD);
	/* A boost/load/thermal limitation may legitimately deliver a lower state. */
	if (((status >> 4) & 15) > 7 || (status & 15) > 7 ||
	    FIELD_GET(APPLE_DVFS_CMD_PS1, command) != fallback)
		return -EIO;
	for (state = 9; state < 16; state++) {
		if (!(g->dirty & BIT(state)))
			continue;
		writeq(g->stock1[state], priv->reg_base + a9_psinfo1_offset(state));
		writeq(g->stock2[state], priv->reg_base + 0x868 + state * 8);
		if (readq(priv->reg_base + a9_psinfo1_offset(state)) != g->stock1[state] ||
		    readq(priv->reg_base + 0x868 + state * 8) != g->stock2[state])
			return -EIO;
		g->dirty &= ~BIT(state);
	}
	g->configured = 0;
	g->delta_uv = 0;
	return 0;
}

static void a9_revert(struct a9_guard *g, const char *reason)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&g->priv->lock, flags);
	g->armed = false;
	g->latched = true;
	g->reason = reason;
	g->revert_error = a9_restore_locked(g);
	raw_spin_unlock_irqrestore(&g->priv->lock, flags);
	if (g->ready)
		freq_qos_update_request(&g->qos, A9_STOCK_KHZ);
	if (g->revert_error)
		dev_err(g->priv->cpu_dev, "A9 stock revert failed: %d\n", g->revert_error);
}

/* Thermal callbacks hold a zone mutex. Do not take g->control here: the
 * polling worker may hold it while acquiring that same zone mutex. RCU keeps
 * the driver state alive and priv->lock serializes the bounded MMIO sequence.
 */
void apple_a9_thermal_revert(void)
{
	struct a9_guard *g;
	unsigned long flags;

	rcu_read_lock();
	g = rcu_dereference(a9_active_guard);
	if (g) {
		raw_spin_lock_irqsave(&g->priv->lock, flags);
		g->trip_generation++;
		g->armed = false;
		g->latched = true;
		g->reason = "thermal-trip";
		g->revert_error = a9_restore_locked(g);
		WRITE_ONCE(g->thermal_qos_pending, true);
		raw_spin_unlock_irqrestore(&g->priv->lock, flags);
		/* QoS notification can sleep; finish it outside this RCU section. */
		mod_delayed_work(system_wq, &g->work, 0);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(apple_a9_thermal_revert);

static const char *a9_guard_fault(int sensor_error, int hottest, bool expired)
{
	struct apple_a9_thermal_profile p = apple_a9_get_thermal_profile();

	if (sensor_error)
		return "sensor-error";
	if (hottest >= p.critical)
		return "thermal-critical";
	if (hottest >= p.hot)
		return "thermal-hot";
	if (hottest >= p.passive)
		return "thermal-passive";
	if (expired)
		return "heartbeat-timeout";
	return NULL;
}

static void a9_guard_work(struct work_struct *work)
{
	struct a9_guard *g = container_of(to_delayed_work(work), struct a9_guard, work);
	int hottest, ret;
	const char *fault;

	mutex_lock(&g->control);
	if (READ_ONCE(g->thermal_qos_pending)) {
		freq_qos_update_request(&g->qos, A9_STOCK_KHZ);
		WRITE_ONCE(g->thermal_qos_pending, false);
	}
	ret = a9_temperatures(&hottest);
	g->hottest = ret ? INT_MIN : hottest;
	if (g->armed) {
		fault = !a9_voltage_handoff_ok(g) ?
			"voltage-mode" : a9_guard_fault(ret, hottest,
			 time_after(jiffies, g->heartbeat + msecs_to_jiffies(A9_HEARTBEAT_MS)));
		if (fault)
			a9_revert(g, fault);
	} else if (!ret && hottest >= apple_a9_get_thermal_profile().hot && !g->latched) {
		a9_revert(g, "thermal-hot");
	}
	mutex_unlock(&g->control);
	mod_delayed_work(system_wq, &g->work, msecs_to_jiffies(A9_POLL_MS));
}

static int a9_check_stock(struct a9_guard *g)
{
	struct cpufreq_frequency_table *p;
	static const u8 core_code[] = { 0, 0, 31, 40, 61, 80, 104, 116, 116 };
	static const u8 aux_code[] = { 0, 0, 64, 64, 64, 64, 77, 101, 101 };
	unsigned int n;

	/* All published stock clocks must match this chip's boot PSINFO words. */
	cpufreq_for_each_valid_entry(p, g->policy->freq_table) {
		n = p->driver_data;
		if (n < 1 || n > 15 || n == 8)
			return -EINVAL;
		if (n >= 9 && a9_step_state(p->frequency) != n)
			return -EINVAL;
		if (n <= 8 && a9_psinfo_khz(g->stock1[n]) != p->frequency)
			return -EINVAL;
	}
	/* This decoder is for the saved S8003 j71t calibration, not arbitrary A9s. */
	for (n = 1; n <= 8; n++)
		if (FIELD_GET(GENMASK_ULL(63, 56), g->stock1[n]) != core_code[n] ||
		    FIELD_GET(GENMASK_ULL(39, 32), g->stock2[n]) != aux_code[n])
			return -EINVAL;
	if (a9_psinfo_khz(g->stock1[8]) != A9_BOOST_KHZ ||
	    (g->stock1[8] >> 56) != (g->stock1[7] >> 56) ||
	    FIELD_GET(GENMASK_ULL(39, 32), g->stock2[8]) !=
	    FIELD_GET(GENMASK_ULL(39, 32), g->stock2[7]) ||
	    FIELD_GET(GENMASK_ULL(43, 40), g->stock2[8]) != 2)
		return -EINVAL;
	/* Slots 9..15 must contain the observed boot placeholder, not other data. */
	for (n = 9; n < 16; n++)
		if (g->stock1[n] != g->stock1[2] || g->stock2[n] != g->stock2[2])
			return -EBUSY;
	return 0;
}

static int a9_arm(struct a9_guard *g)
{
	unsigned long flags;
	u32 generation = READ_ONCE(g->trip_generation);
	int ret, hottest;

	if (!apple_a9_get_thermal_profile().tuning || !g->ready)
		return -EPERM;
	/* Validate the observed cold-boot handoff, without mode/routing writes. */
	if (!a9_voltage_handoff_ok(g))
		return -EOPNOTSUPP;
	ret = a9_check_stock(g);
	if (ret)
		return ret;
	ret = a9_temperatures(&hottest);
	if (ret || hottest >= apple_a9_get_thermal_profile().arm)
		return ret ?: -EBUSY;
	if (g->dirty || g->revert_error || g->armed)
		return -EBUSY;
	raw_spin_lock_irqsave(&g->priv->lock, flags);
	if (!a9_voltage_handoff_ok(g)) {
		raw_spin_unlock_irqrestore(&g->priv->lock, flags);
		return -EOPNOTSUPP;
	}
	if (generation != g->trip_generation) {
		raw_spin_unlock_irqrestore(&g->priv->lock, flags);
		return -EAGAIN;
	}
	g->latched = false;
	g->armed = true;
	g->limit = A9_BOOST_KHZ;
	g->heartbeat = jiffies;
	g->reason = "armed";
	raw_spin_unlock_irqrestore(&g->priv->lock, flags);
	ret = freq_qos_update_request(&g->qos, A9_BOOST_KHZ);
	if (ret < 0)
		a9_revert(g, "qos-error");
	return ret < 0 ? ret : 0;
}

static int a9_configure(struct a9_guard *g, unsigned int khz, unsigned int delta_uv)
{
	struct apple_cpu_priv *priv = g->priv;
	unsigned long flags;
	u64 word1, word2;
	unsigned int state;
	int ret, hottest;

	/* Validate the ceiling before rounding, conversion or any hardware store. */
	if (delta_uv > A9_OVERVOLT_LIMIT_UV)
		return -ERANGE;
	if (khz == A9_BOOST_KHZ && delta_uv)
		return -EINVAL;
	ret = a9_voltage_words(g->stock1[7], g->stock2[7], delta_uv, &word1, &word2);
	if (ret)
		return ret;
	if (!a9_tuning || !g->armed ||
	    time_after(jiffies, g->heartbeat + msecs_to_jiffies(A9_HEARTBEAT_MS)))
		return -EPERM;
	if (!a9_voltage_handoff_ok(g))
		return -EOPNOTSUPP;
	ret = a9_temperatures(&hottest);
	if (ret || hottest >= apple_a9_get_thermal_profile().arm)
		return ret ?: -EBUSY;
	state = a9_step_state(khz);
	if (!state || khz % 24000 || khz > A9_PLL_MAX_KHZ ||
	    FIELD_GET(GENMASK_ULL(17, 13), word1) > 1 ||
	    FIELD_GET(GENMASK_ULL(3, 0), word1))
		return -EINVAL;
	raw_spin_lock_irqsave(&priv->lock, flags);
	/* A thermal callback may have latched while temperatures were sampled. */
	if (!g->armed || time_after(jiffies, g->heartbeat + msecs_to_jiffies(A9_HEARTBEAT_MS))) {
		ret = -EPERM;
		goto unlock;
	}
	if (!a9_voltage_handoff_ok(g)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	ret = a9_restore_locked(g);
	if (ret)
		goto unlock;
	/* Clone the stock all-load 1800 MHz state. Change the PLL multiplier and
	 * both voltage bytes by a bounded equal delta. Timing/load stay stock.
	 * State 8, its load=2 restriction and all Apple states 0..8 are untouched.
	 */
	word1 = (word1 & ~GENMASK_ULL(12, 4)) |
		FIELD_PREP(GENMASK_ULL(12, 4), khz / 24000);
	g->dirty |= BIT(state);
	writeq(word2, priv->reg_base + 0x868 + state * 8);
	writeq(word1, priv->reg_base + a9_psinfo1_offset(state));
	if (readq(priv->reg_base + a9_psinfo1_offset(state)) != word1 ||
	    readq(priv->reg_base + 0x868 + state * 8) != word2) {
		ret = -EIO;
		goto unlock;
	}
	g->configured = BIT(state);
	g->delta_uv = delta_uv;
	g->limit = khz;
	g->heartbeat = jiffies;
unlock:
	raw_spin_unlock_irqrestore(&priv->lock, flags);
	if (ret)
		a9_revert(g, "configure-error");
	else {
		ret = freq_qos_update_request(&g->qos, khz);
		if (ret < 0)
			a9_revert(g, "qos-error");
	}
	return ret < 0 ? ret : 0;
}

static int a9_status_show(struct seq_file *s, void *unused)
{
	struct a9_guard *g = s->private;
	struct apple_a9_thermal_profile p = apple_a9_get_thermal_profile();
	unsigned int n;

	mutex_lock(&g->control);
	seq_printf(s, "format=3 enable=%u armed=%u latched=%u reason=%s limit_khz=%u configured=%x dirty=%x revert_error=%d hottest_millic=%d heartbeat_ms=%u ceiling_delta_uv=%u voltage_writes=psinfo dac_step_uv=%u delta_uv=%u ",
		   a9_tuning, g->armed, g->latched, g->reason, g->limit,
		   g->configured, g->dirty, g->revert_error, g->hottest,
		   jiffies_to_msecs(jiffies - g->heartbeat), A9_OVERVOLT_LIMIT_UV,
		   A9_DAC_STEP_UV, g->delta_uv);
	seq_printf(s, "clock_profile=s8003-r6-shared-slot boost_state=%u thermal_profile_format=1 profile=%s profile_tuning=%u passive_millic=%u hot_millic=%u critical_millic=%u arm_millic=%u cooldown_millic=%u\n",
		   A9_BOOST_STATE,
		   apple_a9_thermal_profile_name(p), p.tuning, p.passive, p.hot,
		   p.critical, p.arm, p.cooldown);
	seq_printf(s, "cmd=%016llx status=%08x actual_khz=%u voltage_ctl=%08x voltage_routes=%08x voltage_ctl_boot=%08x voltage_routes_boot=%08x voltage_handoff_ok=%u handoff_policy=s8003-r4 control_error=%d\n",
		   readq(g->priv->reg_base + APPLE_DVFS_CMD),
		   readl(g->priv->reg_base + APPLE_DVFS_STATUS),
		   apple_soc_cpufreq_get_rate(g->policy->cpu),
		   readl(g->voltage_base), readl(g->voltage_base + 4),
		   g->voltage_ctl_boot, g->voltage_routes_boot,
		   a9_voltage_handoff_ok(g), g->control_error);
	for (n = 1; n < 16; n++) {
		u64 live1 = readq(g->priv->reg_base + a9_psinfo1_offset(n));
		u64 live2 = readq(g->priv->reg_base + 0x868 + n * 8);
		unsigned int core = FIELD_GET(GENMASK_ULL(63, 56), live1);
		unsigned int aux = FIELD_GET(GENMASK_ULL(39, 32), live2);

		seq_printf(s, "state=%u boot_psinfo1=%016llx boot_psinfo2=%016llx boot_khz=%u psinfo1=%016llx psinfo2=%016llx core_code=%u aux_code=%u core_nominal_uv=%u aux_nominal_uv=%u rail_measured=0\n",
			   n, g->stock1[n], g->stock2[n], a9_psinfo_khz(g->stock1[n]),
			   live1, live2, core, aux, A9_CORE_BASE_UV + A9_DAC_STEP_UV * core,
			   A9_AUX_BASE_UV + A9_DAC_STEP_UV * aux);
	}
	mutex_unlock(&g->control);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a9_status);

static ssize_t a9_control_write(struct file *file, const char __user *buffer,
				 size_t count, loff_t *position)
{
	struct a9_guard *g = file->private_data;
	char command[80], tail;
	unsigned int khz, delta;
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buffer, count))
		return -EFAULT;
	if (memchr(command, 0, count))
		return -EINVAL;
	command[count] = 0;
	strim(command);
	mutex_lock(&g->control);
	if (sysfs_streq(command, "revert")) {
		a9_revert(g, "user-revert");
		ret = g->revert_error;
	} else if (sysfs_streq(command, "arm")) {
		ret = a9_arm(g);
	} else if (sysfs_streq(command, "heartbeat")) {
		if (!g->armed)
			ret = -EPERM;
		else if (time_after(jiffies, g->heartbeat + msecs_to_jiffies(A9_HEARTBEAT_MS))) {
			a9_revert(g, "late-heartbeat");
			ret = -ETIMEDOUT;
		} else
			WRITE_ONCE(g->heartbeat, jiffies);
	} else if (sscanf(command, "configure %u %u %c", &khz, &delta, &tail) == 2) {
		ret = a9_configure(g, khz, delta);
		if (ret && (g->armed || g->dirty))
			a9_revert(g, "configure-error");
	} else {
		ret = -EINVAL;
	}
	g->control_error = ret;
	mutex_unlock(&g->control);
	return ret ? ret : count;
}

static const struct file_operations a9_control_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = a9_control_write,
	.llseek = noop_llseek,
};

static int a9_guard_init(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = policy->driver_data;
	struct a9_guard *g;
	unsigned int n;

	if (!of_machine_is_compatible("apple,s8003"))
		return 0;
	g = kzalloc_obj(*g);
	if (!g)
		return -ENOMEM;
	g->voltage_base = ioremap(0x20e0a0000, 0x20);
	if (!g->voltage_base) {
		kfree(g);
		return -ENOMEM;
	}
	g->voltage_ctl_boot = readl(g->voltage_base);
	g->voltage_routes_boot = readl(g->voltage_base + 4);
	g->priv = priv;
	g->policy = policy;
	g->limit = A9_STOCK_KHZ;
	g->reason = "disabled";
	g->hottest = INT_MIN;
	mutex_init(&g->control);
	INIT_DELAYED_WORK(&g->work, a9_guard_work);
	for (n = 0; n < 16; n++) {
		g->stock1[n] = readq(priv->reg_base + a9_psinfo1_offset(n));
		g->stock2[n] = readq(priv->reg_base + 0x868 + n * 8);
	}
	priv->guard = g;
	return 0;
}

static void a9_guard_ready(struct cpufreq_policy *policy)
{
	struct apple_cpu_priv *priv = policy->driver_data;
	struct a9_guard *g = priv->guard;
	int ret;

	if (!g || g->ready)
		return;
	ret = freq_qos_add_request(&policy->constraints, &g->qos, FREQ_QOS_MAX, A9_STOCK_KHZ);
	if (ret < 0) {
		g->reason = "qos-init-error";
		return;
	}
	g->ready = true;
	rcu_assign_pointer(a9_active_guard, g);
	g->directory = debugfs_create_dir("a9_tuning", NULL);
	debugfs_create_file("status", 0400, g->directory, g, &a9_status_fops);
	debugfs_create_file("control", 0200, g->directory, g, &a9_control_fops);
	mod_delayed_work(system_wq, &g->work, msecs_to_jiffies(A9_POLL_MS));
}

static void a9_guard_exit(struct apple_cpu_priv *priv)
{
	struct a9_guard *g = priv->guard;

	if (!g)
		return;
	RCU_INIT_POINTER(a9_active_guard, NULL);
	synchronize_rcu();
	debugfs_remove_recursive(g->directory);
	cancel_delayed_work_sync(&g->work);
	a9_revert(g, "driver-exit");
	if (g->ready)
		freq_qos_remove_request(&g->qos);
	iounmap(g->voltage_base);
	kfree(g);
}

static bool a9_target_allowed(struct apple_cpu_priv *priv, unsigned int state,
			      unsigned int khz)
{
	struct a9_guard *g = priv->guard;

	if (!g)
		return state <= 7 || !priv->a9;
	if (khz > g->limit)
		return false;
	if (state > 8 && (!(g->configured & BIT(state)) || khz != g->limit))
		return false;
	return state <= 7 || (g->armed && a9_tuning && a9_voltage_handoff_ok(g) &&
		!time_after(jiffies, READ_ONCE(g->heartbeat) + msecs_to_jiffies(A9_HEARTBEAT_MS)));
}
#else
static int a9_guard_init(struct cpufreq_policy *policy)
{
	return 0;
}

static void a9_guard_ready(struct cpufreq_policy *policy)
{
}

static void a9_guard_exit(struct apple_cpu_priv *priv)
{
}

static bool a9_target_allowed(struct apple_cpu_priv *priv, unsigned int state,
			      unsigned int khz)
{
	return state <= 7 || !priv->a9;
}
#endif
