/* Copyright (c) 2012-2013, 2020, The Linux Foundation. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * mod by wlkmanist
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mach/cpufreq.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>

#define MSM_THERMAL_TEMP_THRESHOLD_CRIT 115
#define THERMAL_SAFE_DIFF 5

int enabled = 1;
int temp_threshold = 70;
module_param(temp_threshold, int, 0644);

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	const unsigned int safe_diff;
	bool throttling;
	bool pending_change;
} info = {
	.cpuinfo_max_freq = LONG_MAX,
	.limited_max_freq = LONG_MAX,
	.safe_diff = THERMAL_SAFE_DIFF,
	.throttling = false,
	.pending_change = false,
};

struct thermal_levels
{
	uint32_t freq;
	long temp;
}
	thermal_level[] = 
{
	{ 2726400, -THERMAL_SAFE_DIFF },
	{ 2496000, 0 },
	{ 2265600, 1 },
	{ 1958400, 2 },
	{ 1728000, 3 },
	{ 1497600, 4 },
	{ 1267200, 5 },
	{ 1036800, 6 },
	{ 729600,  7 },
	{ 422400,  8 }
};

static struct msm_thermal_data msm_thermal_info;

static struct delayed_work check_temp_work;

int get_threshold(void)
{
	return temp_threshold;
}

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
		return 0;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
		info.limited_max_freq);

	return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
	unsigned int cpu;

	if (info.limited_max_freq == max_freq)
		return;

	info.limited_max_freq = max_freq;

	info.pending_change = true;

	get_online_cpus();
	for_each_online_cpu(cpu)
	{
		cpufreq_update_policy(cpu);
		pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, info.limited_max_freq);
	}
	put_online_cpus();

	info.pending_change = false;
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	uint32_t freq = 0;
	long temp = 0;
	short i;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	if (temp >= MSM_THERMAL_TEMP_THRESHOLD_CRIT)
	{
		pr_info("%s: power down by soc temp limit theshold (%d)\n",
			__func__, MSM_THERMAL_TEMP_THRESHOLD_CRIT);
		sys_sync();
		kernel_power_off();
	}

	if (!enabled) 
	{
		/* if module disabled we need reshedule to check at least once per second 
		 * MSM_THERMAL_TEMP_THRESHOLD_CRIT to avoid permanent hardware damage
		 */
		schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(1000));
		return;
	}

	temp -= temp_threshold;

	if (info.throttling)
	{
		if (temp <  -info.safe_diff)
		{
			limit_cpu_freqs(info.cpuinfo_max_freq);
			info.throttling = false;
			goto reschedule;
		}
		freq = thermal_level[1].freq; /* if throttling active min throttle level is 1, else 0 */
	}

	for (i = 9; i >= info.throttling; i--)
	{
		if (temp >= thermal_level[i].temp)
		{
			freq = thermal_level[i].freq;
			break;
		}
	}

	if (freq)
	{
		limit_cpu_freqs(freq);

		if (!info.throttling)
			info.throttling = true;
	}

reschedule:
	if (temp >= -3 * info.safe_diff)
		schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(40));
	else
		schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(250));
}

int __ref set_enabled(const char *val, const struct kernel_param *kp)
{
	if (*val == '0' || *val == 'n' || *val == 'N')
	{
		enabled = 0; /* disable */
		limit_cpu_freqs(info.cpuinfo_max_freq);
	}
	else
	{
		if (!enabled)
		{
			enabled = 1; /* reschedule */
		} 
	}

	return 0;
}

struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");


static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	ret = of_property_read_u32(node, "qcom,sensor-id", &data.sensor_id);
	if (ret)
		return ret;

	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);

        memcpy(&msm_thermal_info, &data, sizeof(struct msm_thermal_data));

        INIT_DELAYED_WORK(&check_temp_work, check_temp);
        schedule_delayed_work_on(0, &check_temp_work, 5);

	cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
			CPUFREQ_POLICY_NOTIFIER);

	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
	return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.remove = msm_thermal_dev_remove,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

static int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

static void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);
