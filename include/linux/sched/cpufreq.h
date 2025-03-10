/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/cpufreq.h>
#include <linux/types.h>

/*
 * Interface between cpufreq drivers and the scheduler:
 */

#define SCHED_CPUFREQ_IOWAIT	(1U << 0)
#define SCHED_CPUFREQ_MIGRATION	(1U << 1)
#define SCHED_CPUFREQ_INTERCLUSTER_MIG (1U << 3)
#define SCHED_CPUFREQ_WALT (1U << 4)
#define SCHED_CPUFREQ_PL        (1U << 5)
#define SCHED_CPUFREQ_EARLY_DET (1U << 6)
#define SCHED_CPUFREQ_CONTINUE (1U << 8)
#define SCHED_CPUFREQ_FPS (1U << 9)

#ifdef CONFIG_CPU_FREQ
struct cpufreq_policy;

struct update_util_data {
       void (*func)(struct update_util_data *data, u64 time, unsigned int flags);
};

void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
                       void (*func)(struct update_util_data *data, u64 time,
				    unsigned int flags));
void cpufreq_remove_update_util_hook(int cpu);
bool cpufreq_this_cpu_can_update(struct cpufreq_policy *policy);

static inline unsigned long map_util_freq(unsigned long util,
					unsigned long freq, unsigned long cap)
{
	return (freq + (freq >> 2)) * util / cap;
}
#endif /* CONFIG_CPU_FREQ */

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_NOTUTIL)
void sched_cpufreq_governor_change(struct cpufreq_policy *policy,
			struct cpufreq_governor *old_gov);
#else
static inline void sched_cpufreq_governor_change(struct cpufreq_policy *policy,
			struct cpufreq_governor *old_gov) { }
#endif

#endif /* _LINUX_SCHED_CPUFREQ_H */
