/*
 * Read-only observer for bring-up. The caller's nodes follow the same
 * child -> parent order as genpd power transitions. No domain is attached,
 * resumed, pinned on, reset, or written by this function.
 */
int apple_pmgr_observe(struct device_node **nodes, unsigned int count,
		       struct apple_pmgr_observation *states,
		       void (*read)(void *), void *data)
{
	struct platform_device *providers[5];
	struct apple_pmgr_ps *ps[5];
	unsigned long flags;
	unsigned int i, j, found = 0;
	int ret = 0;
	bool accessible = true;

	if (!count || count > ARRAY_SIZE(providers) || !read)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		for (j = 0; j < i; j++)
			if (nodes[i] == nodes[j]) {
				ret = -EINVAL;
				goto put;
			}
		providers[i] = of_find_device_by_node(nodes[i]);
		if (!providers[i]) {
			ret = -EPROBE_DEFER;
			goto put;
		}
		device_lock(&providers[i]->dev);
		found++;
		if (!providers[i]->dev.driver ||
		    strcmp(providers[i]->dev.driver->name, "apple-pmgr-pwrstate")) {
			ret = -EPROBE_DEFER;
			goto put;
		}
		ps[i] = platform_get_drvdata(providers[i]);
		if (!ps[i]) {
			ret = -EPROBE_DEFER;
			goto put;
		}
	}

	/* PMGR domains use GENPD_FLAG_IRQ_SAFE, i.e. genpd.slock. */
	local_irq_save(flags);
	for (i = 0; i < count; i++)
		spin_lock_nested(&ps[i]->genpd.slock, i);
	for (i = 0; i < count; i++) {
		states[i].genpd_status = ps[i]->genpd.status;
		ret = regmap_read(ps[i]->regmap, ps[i]->offset, &states[i].raw);
		if (ret)
			goto unlock;
		if (states[i].genpd_status != GENPD_STATE_ON ||
		    FIELD_GET(APPLE_PMGR_PS_ACTUAL, states[i].raw) != APPLE_PMGR_PS_ACTIVE ||
		    FIELD_GET(APPLE_PMGR_PS_TARGET, states[i].raw) != APPLE_PMGR_PS_ACTIVE ||
		    states[i].raw & (APPLE_PMGR_RESET | APPLE_PMGR_DEV_DISABLE |
				     APPLE_PMGR_PARENT_OFF))
			accessible = false;
	}
	if (accessible)
		read(data);
	else
		ret = -EHOSTDOWN;
unlock:
	for (i = count; i > 0; i--)
		spin_unlock(&ps[i - 1]->genpd.slock);
	local_irq_restore(flags);
put:
	while (found) {
		found--;
		device_unlock(&providers[found]->dev);
		put_device(&providers[found]->dev);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmgr_observe);
