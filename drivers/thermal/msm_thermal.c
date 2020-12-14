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
#include <linux/reboot.h>
#include <linux/syscalls.h>

#define MSM_THERMAL_SAFE_DIFF			5
#define MSM_THERMAL_POLLING_FREQ_PRESET	5

/* Enable thermal throttlong logic		*/
static bool __read_mostly enable_main			= true;

/* Extreme OC (disable soc temp limit)	*/
static bool __read_mostly enable_extreme		= false;

/* Thermal limit (throttling)			*/
static long __read_mostly temp_threshold		= 70;

/* Thermal limit (sync and power off)	*/
static long __read_mostly temp_threshold_crit	= 110;

static unsigned int __read_mostly polling_freq_preset =
				MSM_THERMAL_POLLING_FREQ_PRESET;

module_param(enable_main,			bool, 0644);
module_param(enable_extreme, 		bool, 0444);
module_param(temp_threshold, 		long, 0644);
module_param(temp_threshold_crit, 	long, 0444);
module_param(polling_freq_preset, 	uint, 0644);

const unsigned int polling_val[] = {
	/*   4,   5,   8,  10, 20, 25, 40, : cycles per second */
	0, 250, 200, 125, 100, 50, 40, 25,
};

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	long safe_diff;
	bool throttling;
	bool pending_change;
} info = {
	.cpuinfo_max_freq = LONG_MAX,
	.limited_max_freq = LONG_MAX,
	.safe_diff = MSM_THERMAL_SAFE_DIFF,
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
	{ 2726400,-1 },
	{ 2496000, 0 },
	{ 2265600, 1 },
	{ 1958400, 2 },
	{ 1728000, 3 },
	{ 1497600, 4 },
	{ 1267200, 5 },
	{ 1036800, 6 },
	{ 729600,  8 },
	{ 422400, 10 }
};

static struct msm_thermal_data msm_thermal_info;

static struct delayed_work check_temp_work;

inline unsigned long get_polling_interval_jiffies(void)
{
	if (unlikely(!polling_freq_preset && polling_freq_preset > 7))
	{
		pr_info("%s: Restore polling_freq_preset to default", KBUILD_MODNAME);
		polling_freq_preset = MSM_THERMAL_POLLING_FREQ_PRESET;
	}

	return msecs_to_jiffies(polling_val[polling_freq_preset]);
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

	if (info.limited_max_freq != info.cpuinfo_max_freq)
		pr_debug("%s: CPU freq limit (%d)\n",
					KBUILD_MODNAME, info.limited_max_freq);
	else pr_debug("%s: Restore CPU freq", KBUILD_MODNAME);

	get_online_cpus();
	for_each_online_cpu(cpu) cpufreq_update_policy(cpu);
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

	if (unlikely(temp >= temp_threshold_crit && !enable_extreme))
	{
		pr_err("%s: Power off. Critical SOC temperature (%ld).\n",
						KBUILD_MODNAME, temp_threshold_crit);
		sys_sync();
		kernel_power_off();
	}

	if (unlikely(!enable_main))
	{
		/* if module disabled we need reshedule to check at least once per
		 * second temp_threshold_crit value to avoid permanent hardware damage
		 */
		schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(1000));
		return;
	}

	temp -= temp_threshold;

	if (temp < -info.safe_diff)
	{
		if (unlikely(info.throttling))
		{
			limit_cpu_freqs(info.cpuinfo_max_freq);
			info.throttling = false;
		}
		goto reschedule;
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
		schedule_delayed_work_on(0, &check_temp_work,
						get_polling_interval_jiffies());
	else
		schedule_delayed_work_on(0, &check_temp_work,
						msecs_to_jiffies(250));
}

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
