// SPDX-License-Identifier: GPL-2.0
//
// Unisoc cpufreq driver public data
//
// Copyright (C) 2021 Unisoc, Inc.
//
// Trimmed vs realme 5.4: the "cpufreq-clus0" branch serviced the retired
// old-generation sprd-cpufreqhw driver and is dropped; only the hwdvfs
// policy layer ("opp-table0") remains.

#include "sprd-hwdvfs-cpufreq.h"

static unsigned int sprd_cpufreq_update_normal(struct cpufreq_policy *policy,
					       int cpu, int temp)
{
	struct sprd_cpufreq_info *info = NULL;

	info = policy->driver_data;
	if (IS_ERR_OR_NULL(info) || !info->update_opp)
		return 0;

	return info->update_opp(cpu, temp);
}

unsigned int sprd_cpufreq_update_opp(int cpu, int temp_now)
{
	struct device_node *cpu_np, *cpufreq_np;
	struct device *cpu_dev;
	struct cpufreq_policy *policy;
	unsigned int max_freq = 0;

	/* cpufreq_get_policy() is gone in 7.1; take a policy reference */
	policy = cpufreq_cpu_get(0);
	if (!policy) {
		pr_debug("%s: No cpu data found\n", __func__);
		return 0;
	}

	cpu_dev = get_cpu_device(0);
	if (!cpu_dev) {
		pr_err("%s: Failed to get cpu0 device\n", __func__);
		goto put_policy;
	}

	cpu_np = of_node_get(cpu_dev->of_node);
	if (!cpu_np) {
		pr_err("%s: Failed to find cpu node\n", __func__);
		goto put_policy;
	}

	cpufreq_np = of_parse_phandle(cpu_np, "cpufreq-data-v1", 0);
	if (!cpufreq_np) {
		pr_err("%s: No cpufreq data found for cpu0\n", __func__);
		of_node_put(cpu_np);
		goto put_policy;
	}

	pr_debug("%s: cluster name is: %s\n", __func__, cpufreq_np->full_name);

	if (!strcmp(cpufreq_np->full_name, "opp-table0"))
		max_freq = sprd_cpufreq_update_normal(policy, cpu, temp_now);
	else
		pr_err("%s: Error name of cpufreq data v1!\n", __func__);

	of_node_put(cpufreq_np);
	of_node_put(cpu_np);
put_policy:
	cpufreq_cpu_put(policy);

	return max_freq;
}
EXPORT_SYMBOL_GPL(sprd_cpufreq_update_opp);

MODULE_LICENSE("GPL v2");
