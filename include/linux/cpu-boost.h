// by wlkmanist
// 28 mar 2020



#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>

#ifndef _CPU_BOOST_
#define _CPU_BOOST_
#ifdef CONFIG_CPUBOOST

struct cpu_sync {
	struct task_struct *thread;
	wait_queue_head_t sync_wq;
	struct delayed_work boost_rem;
	struct delayed_work input_boost_rem;
	int cpu;
	spinlock_t lock;
	bool pending;
	atomic_t being_woken;
	int src_cpu;
	unsigned int boost_min;
	unsigned int input_boost_min;
};

struct cpu_sync *get_actual_sync_info(void);

#endif
#endif
