/*
 * Author: Park Ju Hyung aka arter97 <qkrwngud825@gmail.com>
 * Base intelli_plug author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * Copyright 2012~2014 Paul Reioux
 * Copyright 2015 Park Ju Hyung
 *
 *
 ** Introduction
 *
 * Other hotplugging methods including mpdecision and intelli_plug focuses
 * on how should we turn off CPU cores. They hotplugs the individual CPU
 * cores based on the current load divided by thread capacity.
 * Lazyplug takes a whole new approach on how we should do hotplugging
 * based on the foundation of the other side of the coin;
 * “Linux’s hotplugging is very inefficient.”
 *
 * Current hotplugging code on Linux is a total waste of CPU cycles and
 * delays, so rather than hotplugging and hurt performance & battery life,
 * just leaving the CPU cores on might be a better choice. This kind of
 * approach is spreading out more and more.
 * Samsung has been using this method for a very long time with big.LITTLE
 * devices and recent Nexus 6 firmware also does the similar thing.
 *
 * Lazyplug just leaves them on, most of the time. It also tries to solve
 * some problems with the “Always on” approach. On situations such as video
 * playback, turning on all CPU cores is not battery friendly. So Lazyplug
 * *does* actually turns off CPU cores, but only when idle state is long
 * enough(to reduce the number of CPU core switchings) and when the device
 * has its screen off(determination is done via powersuspend or
 * powersuspend because framebuffer API causes troubles on hotplugging CPU
 * cores).
 *
 * Basic methodology :
 * Lazyplug uses majority of the codes from intelli_plug by faux123 to
 * determine when to turn off CPU cores. If the system has been idle for
 * (DEF_SAMPLING_MS * DEF_IDLE_COUNT)ms, it turns off the CPU cores. And if
 * the next poll determines 1 core isn’t enough, it fires up all CPU cores
 * (instead of selective CPU cores; which is the traditional intelli_plug’s
 * method).
 * Lazyplug also takes touch-screen input events to fire up CPU cores to
 * minimize noticeable performance degradation.
 * There’s also a “lazy mode” for *not* aggressively turning on CPU cores
 * on scenario such as video playback. For example, if you hook up
 * lazyplug_enter_lazy() to the video session open function, Lazyplug won’t
 * aggressively turn on CPU cores and tries to handle it with 1 CPU core.
 *
 ** TODO :
 ** Dual-core mode : YouTube video playback is mostly single-threaded.
 ** It usually hovers around 10% ~ 30% of total CPU usage on quad-core
 ** device. That means 1 CPU core might not be enough to handle it, but
 ** also turning on all CPU cores is unnecessarily wasting power.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/cpufreq.h>
#include <linux/display_state.h>
#include <linux/powersuspend.h>
#include <linux/omniplug.h>
#include <linux/omniboost.h>


//#define DEBUG_LAZYPLUG
#undef DEBUG_LAZYPLUG

#define LAZYPLUG_MAJOR_VERSION	1
#define LAZYPLUG_MINOR_VERSION	0

#define DEF_SAMPLING_MS			(268)
#define DEF_IDLE_COUNT			(19) /* 268 * 19 = 5092, almost equals to 5 seconds */

#define DUAL_PERSISTENCE		(2500 / DEF_SAMPLING_MS)
#define TRI_PERSISTENCE			(1700 / DEF_SAMPLING_MS)
#define QUAD_PERSISTENCE		(1000 / DEF_SAMPLING_MS)

#define BUSY_PERSISTENCE		(3500 / DEF_SAMPLING_MS)

static DEFINE_MUTEX(lazyplug_mutex);
static DEFINE_MUTEX(lazymode_mutex);

static struct delayed_work lazyplug_work;
static struct delayed_work lazyplug_boost;

static struct workqueue_struct *lazyplug_wq;
static struct workqueue_struct *lazyplug_boost_wq;

static unsigned int lazyplug_active = 0;

static unsigned int __read_mostly touch_boost_active = 1;
module_param(touch_boost_active, uint, 0664);

static unsigned int nr_run_profile_sel = 0;
module_param(nr_run_profile_sel, uint, 0664);

/* default to something sane rather than zero */
static unsigned int __read_mostly sampling_time = DEF_SAMPLING_MS;

static int persist_count = 0;

struct ip_cpu_info {
	unsigned int sys_max;
	unsigned int cur_max;
	unsigned long cpu_nr_running;
};

static DEFINE_PER_CPU(struct ip_cpu_info, ip_info);

#define CAPACITY_RESERVE	50

#define THREAD_CAPACITY	(250 - CAPACITY_RESERVE)

#define MULT_FACTOR	4
#define DIV_FACTOR	100000
#define NR_FSHIFT	3

static unsigned int nr_fshift = NR_FSHIFT;

static unsigned int __read_mostly nr_run_thresholds_balance[] = {
	(THREAD_CAPACITY * 625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 1125 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int __read_mostly nr_run_thresholds_performance[] = {
	(THREAD_CAPACITY * 380 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int __read_mostly nr_run_thresholds_conservative[] = {
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 1625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 2125 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int __read_mostly nr_run_thresholds_eco[] = {
        (THREAD_CAPACITY * 380 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int __read_mostly nr_run_thresholds_eco_extreme[] = {
        (THREAD_CAPACITY * 750 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int __read_mostly nr_run_thresholds_disable[] = {
	0,  0,  0,  UINT_MAX
};

static unsigned int __read_mostly *nr_run_profiles[] = {
	nr_run_thresholds_balance,
	nr_run_thresholds_performance,
	nr_run_thresholds_conservative,
	nr_run_thresholds_eco,
	nr_run_thresholds_eco_extreme,
	nr_run_thresholds_disable,
};

#define NR_RUN_ECO_MODE_PROFILE	3
#define NR_RUN_HYSTERESIS_QUAD	8
#define NR_RUN_HYSTERESIS_DUAL	4

#define CPU_NR_THRESHOLD	((THREAD_CAPACITY << 1) + (THREAD_CAPACITY / 2))

static unsigned int __read_mostly cpu_nr_run_threshold = CPU_NR_THRESHOLD;
module_param(cpu_nr_run_threshold, uint, 0664);

static unsigned int __read_mostly nr_run_hysteresis = NR_RUN_HYSTERESIS_QUAD;
module_param(nr_run_hysteresis, uint, 0664);

static unsigned int nr_run_last;

static unsigned int idle_count = 0;

extern unsigned long avg_nr_running(void);
extern unsigned long avg_cpu_nr_running(unsigned int cpu);

static void cpu_all_ctrl(bool online) {
	unsigned int cpu;

	if (!is_display_on() || !lazyplug_active)
		return;

	hardplug_all_cpus();

	if (online) {
		for_each_nonboot_offline_cpu(cpu) {
			if (cpu_out_of_range_hp(cpu))
				break;
			if (cpu_online(cpu) ||
				!is_cpu_allowed(cpu) ||
				thermal_core_controlled(cpu))
				continue;
			cpu_up(cpu);
		}
	} else {
		for_each_nonboot_online_cpu(cpu) {
			if (cpu_out_of_range_hp(cpu))
				break;
			if (!cpu_online(cpu))
				continue;
			cpu_down(cpu);
		}
	}
}

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run;
	unsigned int threshold_size;
	unsigned int *current_profile;

	current_profile = nr_run_profiles[nr_run_profile_sel];
	if (num_possible_cpus() > 2) {
		if (nr_run_profile_sel >= NR_RUN_ECO_MODE_PROFILE)
			threshold_size =
				ARRAY_SIZE(nr_run_thresholds_eco);
		else
			threshold_size =
				ARRAY_SIZE(nr_run_thresholds_balance);
	} else
		threshold_size =
			ARRAY_SIZE(nr_run_thresholds_eco);

	if (nr_run_profile_sel >= NR_RUN_ECO_MODE_PROFILE)
		nr_fshift = 1;
	else
		nr_fshift = num_possible_cpus() - 1;

	for (nr_run = 1; nr_run < threshold_size; nr_run++) {
		unsigned int nr_threshold;
		nr_threshold = current_profile[nr_run - 1];

		if (nr_run_last <= nr_run)
			nr_threshold += nr_run_hysteresis;
		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)))
			break;
	}
	nr_run_last = nr_run;

	return nr_run;
}

static void lazyplug_boost_fn(struct work_struct *work)
{
	if (!is_display_on() || !lazyplug_active)
		return;
	cpu_all_ctrl(true);
}

/*
static int cmp_nr_running(const void *a, const void *b)
{
	return *(unsigned long *)a - *(unsigned long *)b;
}
*/

static void update_per_cpu_stat(void)
{
	unsigned int cpu;
	struct ip_cpu_info *l_ip_info;

	if (!is_display_on() || !lazyplug_active)
		return;

	for_each_online_cpu(cpu) {
		l_ip_info = &per_cpu(ip_info, cpu);
		l_ip_info->cpu_nr_running = avg_cpu_nr_running(cpu);
#ifdef DEBUG_LAZYPLUG
		pr_info("cpu %u nr_running => %lu\n", cpu,
			l_ip_info->cpu_nr_running);
#endif
	}
}

static void unplug_cpu(int min_active_cpu)
{
	unsigned int cpu;
	struct ip_cpu_info *l_ip_info;
	int l_nr_threshold;

	if (!is_display_on() || !lazyplug_active)
		return;

	for_each_nonboot_online_cpu(cpu) {
		if (cpu_out_of_range_hp(cpu))
			break;
		if (!cpu_online(cpu))
			continue;
		l_nr_threshold =
			cpu_nr_run_threshold << 1 / (num_online_cpus());
		l_ip_info = &per_cpu(ip_info, cpu);
		if (cpu > min_active_cpu) {
			if (l_ip_info->cpu_nr_running < l_nr_threshold)
				cpu_down(cpu);
		}
	}
}

static void lazyplug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat;
	unsigned int cpu_count = 0;
	unsigned int nr_cpus = 0;

	if (!is_display_on() || !lazyplug_active)
		return;

	hardplug_all_cpus();
	nr_run_stat = calculate_thread_stats();
	update_per_cpu_stat();
	cpu_count = nr_run_stat;
	nr_cpus = num_online_cpus();

	if (persist_count > 0)
		persist_count--;

	if (cpu_count == 1) {
		/* start counting idle states */
		if (idle_count < DEF_IDLE_COUNT)
			idle_count++;

		if (idle_count == DEF_IDLE_COUNT && persist_count == 0) {
			/* take down everyone */
			unplug_cpu(0);
#ifdef DEBUG_LAZYPLUG
			offline_state_count++;
			if (previous_online_status == true) {
				previous_online_status = false;
				switch_count++;
			}
		} else {
			online_state_count++;
			if (previous_online_status == false) {
				previous_online_status = true;
				switch_count++;
			}
#endif
		}
	} else {
		idle_count = 0;
		cpu_all_ctrl(true);
	}

resched:
	queue_delayed_work_on(0, lazyplug_wq, &lazyplug_work,
		msecs_to_jiffies(sampling_time));
}

static void wakeup_boost_lazy(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	struct ip_cpu_info *l_ip_info;

	if (!is_display_on() || !lazyplug_active)
		return;

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get_raw(cpu);
		if (policy != NULL) {
			l_ip_info = &per_cpu(ip_info, cpu);
			policy->cur = l_ip_info->cur_max;
			cpufreq_update_policy(cpu);
		}
	}
}

static void cpu_all_up(struct work_struct *work);
static DECLARE_WORK(cpu_all_up_work, cpu_all_up);

static void cpu_all_up(struct work_struct *work)
{
	if (!is_display_on() || !lazyplug_active)
		return;

	cpu_all_ctrl(true);
	wakeup_boost_lazy();
}

static unsigned int Lnr_run_profile_sel = 0;
static unsigned int Ltouch_boost_active = true;
static bool Lprevious_state = false;
void lazyplug_enter_lazy(bool enter)
{
	if (!is_display_on() || !lazyplug_active)
		return;

	mutex_lock(&lazymode_mutex);
	if (enter && !Lprevious_state) {
		pr_info("lazyplug: entering lazy mode\n");
		Lnr_run_profile_sel = nr_run_profile_sel;
		Ltouch_boost_active = touch_boost_active;
		nr_run_profile_sel = 2; /* conversative profile */
		touch_boost_active = false;
		Lprevious_state = true;
	} else if (!enter && Lprevious_state) {
		pr_info("lazyplug: exiting lazy mode\n");
		touch_boost_active = Ltouch_boost_active;
		nr_run_profile_sel = Lnr_run_profile_sel;
		Lprevious_state = false;
	}
	mutex_unlock(&lazymode_mutex);
}

static void lazyplug_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{

}
static int lazy_omniboost_notifier(struct notifier_block *self, unsigned long val,
		void *v)
{
	if (!is_display_on() || !lazyplug_active)
		return NOTIFY_OK;

	switch (val) {
	case BOOST_ON:
		if (lazyplug_active && touch_boost_active) {
			idle_count = 0;
			queue_delayed_work_on(0, lazyplug_wq, &lazyplug_boost,
				msecs_to_jiffies(10));
		}
		break;
	case BOOST_OFF:
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block lazy_nb = {
	.notifier_call = lazy_omniboost_notifier,
};

static void lazy_suspend(struct power_suspend * h)
{
}
static void lazy_resume(struct power_suspend * h)
{
		queue_delayed_work_on(0, lazyplug_wq, &lazyplug_work,
			msecs_to_jiffies(10));
}

static struct power_suspend lazy_suspend_data =
{
	.suspend = lazy_suspend,
	.resume = lazy_resume,
};

void start_stop_lazy_plug(unsigned int enabled)
{
	int rc;

	lazyplug_active = enabled;
	if (enabled) {
		lazyplug_wq = alloc_workqueue("lazyplug",
					WQ_HIGHPRI | WQ_UNBOUND, 1);
		lazyplug_boost_wq = alloc_workqueue("lplug_boost",
					WQ_HIGHPRI | WQ_UNBOUND, 1);
		INIT_DELAYED_WORK(&lazyplug_work, lazyplug_work_fn);
		INIT_DELAYED_WORK(&lazyplug_boost, lazyplug_boost_fn);
		register_power_suspend(&lazy_suspend_data);
		queue_delayed_work_on(0, lazyplug_wq, &lazyplug_work,
			msecs_to_jiffies(10));
		register_omniboost(&lazy_nb);
	} else if (!enabled) {
		unregister_omniboost(&lazy_nb);
		unregister_power_suspend(&lazy_suspend_data);
		cancel_delayed_work_sync(&lazyplug_boost);
		cancel_delayed_work_sync(&lazyplug_work);
		destroy_workqueue(lazyplug_boost_wq);
		destroy_workqueue(lazyplug_wq);
	}
}

int __init lazyplug_init(void)
{
	int rc;

	pr_info("lazyplug: version %d.%d by arter97\n"
		"          based on intelli_plug by faux123\n",
		 LAZYPLUG_MAJOR_VERSION,
		 LAZYPLUG_MINOR_VERSION);


	nr_run_hysteresis = NR_RUN_HYSTERESIS_QUAD;
	nr_run_profile_sel = 0;

	if (lazyplug_active)
		start_stop_lazy_plug(lazyplug_active);

	return 0;
}

late_initcall(lazyplug_init);

MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
MODULE_LICENSE("GPL");
