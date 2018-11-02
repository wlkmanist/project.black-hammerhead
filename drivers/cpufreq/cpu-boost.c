/*
 * Copyright (c) 2013-2015,2020-2021, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#include <linux/cpu-boost.h>

#ifdef CONFIG_STATE_NOTIFIER
#include <linux/state_notifier.h>
#endif

#ifdef CONFIG_THERMAL_MONITOR
#include <linux/msm_thermal.h>
#endif

static DEFINE_PER_CPU(struct cpu_sync, sync_info);

/* Workqueue used to run boosting algorithms on */
static struct workqueue_struct *cpu_boost_wq;

/* Instant input boosting work */
static struct work_struct input_boost_work;

/*
 * Time in milliseconds to keep frequencies of source and destination cpus
 * synchronized after the task migration event between them reported by sched.
 */
static unsigned int __read_mostly boost_ms;
module_param(boost_ms, uint, 0644);

/*
 * Boolean to determine whether the module should react to all task migration
 * events or only to those which maintain task load at least that specified by
 * migration_load_threshold. This variable also changes the way CPU frequencies
 * are going to be changed: when it is set to false, frequencies of source and
 * destination cpus are simply synchronized to a source's one; in case this is
 * set to true, the frequency is changed to either the load fraction of current
 * policy maximum or source's frequency, choosing the biggest of two.
 */
static bool __read_mostly load_based_syncs = true;
module_param(load_based_syncs, bool, 0644);

/*
 * Minimum task load that is considered as noticeable. If a task load is less
 * than this value, frequency synchronization will not occur.  Note that this
 * threshold is used only if load_based_syncs is enabled.
 */
static unsigned int __read_mostly migration_load_threshold = 30;
module_param(migration_load_threshold, uint, 0644);

/*
 * Frequency cap for synchronization algorithm. Shared frequency of synchronized
 * cpus will not go above this threshold if it is set to non-zero value.
 */
static unsigned int __read_mostly sync_threshold;
module_param(sync_threshold, uint, 0644);

static unsigned int __read_mostly sync_threshold_min;
module_param(sync_threshold_min, uint, 0644);

static unsigned int __read_mostly input_boost_freq;
module_param(input_boost_freq, uint, 0644);

/*
 * Time in milliseconds to keep frequencies of all online cpus boosted after an
 * input event.  Note that multiple input events, that occurred during the time
 * interval which is less or equal to min_input_interval, will be accounted as
 * one.
 */
static unsigned int __read_mostly input_boost_ms = 0;
module_param(input_boost_ms, uint, 0644);

/*
 * Time to boost cpu to max frequency when an app is launched
 */
static unsigned int __read_mostly app_launch_boost_ms = 1500;
module_param(app_launch_boost_ms, uint, 0644);

#ifdef CONFIG_STATE_NOTIFIER
static bool __read_mostly disable_while_suspended = false;
module_param(disable_while_suspended, bool, 0644);
#endif

#ifdef CONFIG_THERMAL_MONITOR
extern struct thermal_info cpu_thermal_info;
#endif

static u64 last_input_time;
#define MIN_INPUT_INTERVAL (150 * USEC_PER_MSEC)

struct cpu_sync *get_actual_sync_info(void)
{
	return &sync_info;
}

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 *
 * The sync kthread needs to run on the CPU in question to avoid deadlocks in
 * the wake up code. Achieve this by binding the thread to the respective
 * CPU. But a CPU going offline unbinds threads from that CPU. So, set it up
 * again each time the CPU comes back up. We can use CPUFREQ_START to figure
 * out a CPU is coming online instead of registering for hotplug notifiers.
 */
static int boost_adjust_notify(struct notifier_block *nb,
	unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int b_min = s->boost_min;
	unsigned int ib_min = s->input_boost_min;
	unsigned int min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!b_min && !ib_min)
			break;

		ib_min = min((s->input_boost_min == UINT_MAX ?
			policy->max : s->input_boost_min), policy->max);

		/*
		 * If we're not resetting the boost and if the new boosted freq
		 * is below or equal to the current min freq, bail early
		 */
		if (ib_min) {
			if (ib_min <= policy->min)
				break;
		}

		min = max(b_min, ib_min);

		pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("CPU%u boost min: %u kHz\n", cpu, min);

		cpufreq_verify_within_limits(policy, min, UINT_MAX);

		pr_debug("CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);
		break;

	case CPUFREQ_START:
		set_cpus_allowed(s->thread, *cpumask_of(cpu));
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static void do_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s = container_of(work, struct cpu_sync,
						boost_rem.work);

	pr_debug("Removing boost for CPU%d\n", s->cpu);
	s->boost_min = 0;
	/* Force policy re-evaluation to trigger adjust notifier. */
	cpufreq_update_policy(s->cpu);
}

static void do_input_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s = container_of(work, struct cpu_sync,
						input_boost_rem.work);

	pr_debug("Removing input boost for CPU%d\n", s->cpu);
	s->input_boost_min = 0;
	/* Force policy re-evaluation to trigger adjust notifier. */
	cpufreq_update_policy(s->cpu);
}

static int boost_mig_sync_thread(void *data)
{
	int dest_cpu = (int)data, src_cpu, ret;
	struct cpu_sync *s = &per_cpu(sync_info, dest_cpu);
	struct cpufreq_policy dest_policy, src_policy;
	unsigned long flags;
	unsigned int req_freq;

	for (;;) {
		wait_event_interruptible(s->sync_wq, s->pending ||
					kthread_should_stop());

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&s->lock, flags);
		s->pending = false;
		src_cpu = s->src_cpu;
		spin_unlock_irqrestore(&s->lock, flags);

		ret  = cpufreq_get_policy(&src_policy, src_cpu);
		ret |= cpufreq_get_policy(&dest_policy, dest_cpu);
		if (IS_ERR_VALUE(ret))
			continue;

		req_freq = max(dest_policy.max * s->task_load / 100, src_policy.cur);

		if (sync_threshold)
			req_freq = min(req_freq, sync_threshold);

#ifdef CONFIG_THERMAL_MONITOR
		if (unlikely(cpu_thermal_info.throttling &&
					req_freq > cpu_thermal_info.limited_max_freq))
			req_freq = min(req_freq, cpu_thermal_info.limited_max_freq);
#endif

		if (unlikely(req_freq <= dest_policy.cpuinfo.min_freq))
			continue;

		if (sync_threshold_min && req_freq < sync_threshold_min)
			continue;

		if (delayed_work_pending(&s->boost_rem))
			cancel_delayed_work_sync(&s->boost_rem);

		s->boost_min = req_freq;

		/* Force policy re-evaluation to trigger adjust notifier. */
		get_online_cpus();
		if (likely(cpu_online(src_cpu))) {
			/*
			 * Send an unchanged policy update to the source
			 * CPU. Even though the policy isn't changed from
			 * its existing boosted or non-boosted state
			 * notifying the source CPU will let the governor
			 * know a boost happened on another CPU and that it
			 * should re-evaluate the frequency at the next timer
			 * event without interference from a min sample time.
			 */
			cpufreq_update_policy(src_cpu);
		}

		if (likely(cpu_online(dest_cpu))) {
			cpufreq_update_policy(dest_cpu);
			queue_delayed_work_on(dest_cpu, cpu_boost_wq,
				&s->boost_rem, msecs_to_jiffies(boost_ms));
		} else {
			s->boost_min = 0;
		}
		put_online_cpus();
	}

	return 0;
}

static int boost_migration_notify(struct notifier_block *nb,
				unsigned long dest_cpu, void *arg)
{
	unsigned long flags;
	struct cpu_sync *s = &per_cpu(sync_info, dest_cpu);
	struct cpufreq_policy src_policy;

#ifdef CONFIG_STATE_NOTIFIER
	if (unlikely(state_suspended && disable_while_suspended))
		return NOTIFY_OK;
#endif

	if (!boost_ms)
		return NOTIFY_OK;

	if (unlikely(cpufreq_get_policy(&src_policy, (unsigned int) arg)))
		pr_err("%s: Failed to get cpu policy.\n", KBUILD_MODNAME);
	else
		if (load_based_syncs && src_policy.util < migration_load_threshold)
			return NOTIFY_OK;

	/* Avoid deadlock in try_to_wake_up() */
	if (unlikely(s->thread == current))
		return NOTIFY_OK;

	pr_debug("Migration: CPU%d --> CPU%d\n", (int) arg, (int) dest_cpu);
	spin_lock_irqsave(&s->lock, flags);
	s->pending = true;
	s->src_cpu = (int) arg;
	s->task_load = load_based_syncs ? src_policy.util : 0;
	spin_unlock_irqrestore(&s->lock, flags);
	/*
	* Avoid issuing recursive wakeup call, as sync thread itself could be
	* seen as migrating triggering this notification. Note that sync thread
	* of a cpu could be running for a short while with its affinity broken
	* because of CPU hotplug.
	*/
	if (likely(!atomic_cmpxchg(&s->being_woken, 0, 1))) {
		wake_up(&s->sync_wq);
		atomic_set(&s->being_woken, 0);
	}

	return NOTIFY_OK;
}

static struct notifier_block boost_migration_nb = {
	.notifier_call = boost_migration_notify,
};

void do_app_launch_boost()
{
	unsigned int i;
	struct cpu_sync *i_sync_info;

	if (!app_launch_boost_ms)
		return;

 	cancel_delayed_work_sync(&i_sync_info->input_boost_rem);

 	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = UINT_MAX;
	}

 	update_policy_online();

 	queue_delayed_work(cpu_boost_wq, &i_sync_info->input_boost_rem,
		msecs_to_jiffies(app_launch_boost_ms));
}

static void do_input_boost(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;
	struct cpufreq_policy policy;

	get_online_cpus();
	for_each_online_cpu(i) {

		i_sync_info = &per_cpu(sync_info, i);
		ret = cpufreq_get_policy(&policy, i);
		if (ret)
			continue;
		if (policy.cur >= input_boost_freq)
			continue;

		cancel_delayed_work_sync(&i_sync_info->input_boost_rem);
#ifdef CONFIG_THERMAL_MONITOR
		if (unlikely(cpu_thermal_info.throttling &&
					input_boost_freq > cpu_thermal_info.limited_max_freq))
			i_sync_info->input_boost_min = cpu_thermal_info.limited_max_freq;
		else
#endif
		i_sync_info->input_boost_min = input_boost_freq;
		cpufreq_update_policy(i);
		queue_delayed_work_on(i_sync_info->cpu, cpu_boost_wq,
			&i_sync_info->input_boost_rem,
			msecs_to_jiffies(input_boost_ms));
	}
	put_online_cpus();
}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

#ifdef CONFIG_STATE_NOTIFIER
	if (unlikely(state_suspended && disable_while_suspended))
		return;
#endif

	if (unlikely(!input_boost_freq))
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < MIN_INPUT_INTERVAL)
		return;

	if (work_pending(&input_boost_work))
		return;

	queue_work(cpu_boost_wq, &input_boost_work);
	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int cpu_boost_init(void)
{
	int cpu, ret;
	struct cpu_sync *s;

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (!cpu_boost_wq)
		return -EFAULT;

	INIT_WORK(&input_boost_work, do_input_boost);

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
		init_waitqueue_head(&s->sync_wq);
		atomic_set(&s->being_woken, 0);
		spin_lock_init(&s->lock);
		INIT_DELAYED_WORK(&s->boost_rem, do_boost_rem);
		INIT_DELAYED_WORK(&s->input_boost_rem, do_input_boost_rem);
		s->thread = kthread_run(boost_mig_sync_thread, (void *)cpu,
					"boost_sync/%d", cpu);
		set_cpus_allowed(s->thread, *cpumask_of(cpu));
	}
	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	atomic_notifier_chain_register(&migration_notifier_head,
					&boost_migration_nb);
	ret = input_register_handler(&cpuboost_input_handler);

	return 0;
}
late_initcall(cpu_boost_init);
