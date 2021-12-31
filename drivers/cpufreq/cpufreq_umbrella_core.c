/*
 * drivers/cpufreq/cpufreq_umbrella_core.c
 *
 * Copyright (C) 2010 Google, Inc.
 *           (C) 2014-2021 AbandonedCart <twistedumbrella@gmail.com>
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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/percpu-defs.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
#include <asm/cputime.h>

#ifndef cputime_to_nsecs
# define cputime_to_nsecs(__ct)    \
    (cputime_to_usecs(__ct) * NSEC_PER_USEC)
#endif
#endif /* CONFIG_VIRT_CPU_ACCOUNTING_NATIVE */

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_umbrella_core.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#define CONFIG_UC_MODE_AUTO_CHANGE
#define CONFIG_UC_MODE_AUTO_CHANGE_BOOST

#define gov_attr_ro(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define gov_attr_wo(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0200, NULL, store_##_name)

#define gov_attr_rw(_name)						\
static struct governor_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

static int active_count;

struct umbrella_core_cpuinfo {
  struct umbrella_core_policy *ipolicy;
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int target_freq;
	unsigned int floor_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time;
	struct rw_semaphore enable_sem;
	int governor_enabled;
	int prev_load;
	bool limits_changed;
	unsigned int nr_timer_resched;
};

static DEFINE_PER_CPU(struct umbrella_core_cpuinfo, umbrella_core_cpuinfo);

#define MIN_TIMER_JIFFIES 1UL

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

/* Freq Table
 * ----------------------------
 * policy0   policy4   policy7
 * ----------------------------
 * 300000    710400    844800
 * 403200    844800    960000
 * 499200    960000    1075200
 * 595200    1075200   1190400
 * 691200    1209600   1305600
 * 806400    1324800   1420800
 * 902400    1440000   1555200
 * 998400    1555200   1670400
 * 1094400   1670400   1785600
 * 1209600   1766400   1900800
 * 1305600   1881600   2035200
 * 1401600   1996800   2150400
 * 1497600   2112000   2265600
 * 1612800   2227200   2380800
 * 1708800   2342400   2496000
 * 1804800   2419200   2592000
 *                     2688000
 *                     2764800
 *                     2841600
 */

struct umbrella_core_tunables {
	struct gov_attr_set attr_set;
  /* Hi speed to bump to from lo speed when load burst (default max) */
	unsigned int hispeed_freq;

  /* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD_MIN 85
#define DEFAULT_GO_HISPEED_LOAD_MID 90
#define DEFAULT_GO_HISPEED_LOAD_MAX 95
	unsigned long go_hispeed_load;

  /* Sampling down factor to be applied to min_sample_time at max freq */
	unsigned int sampling_down_factor;

  /* Target load.  Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;

  /*
   * The minimum amount of time to spend at a frequency before we can ramp down.
   */
 #define DEFAULT_MIN_SAMPLE_TIME_MIN (80 * USEC_PER_MSEC)
 #define DEFAULT_MIN_SAMPLE_TIME_MID (60 * USEC_PER_MSEC)
 #define DEFAULT_MIN_SAMPLE_TIME_MAX (40 * USEC_PER_MSEC)
	unsigned long min_sample_time;

  /*
   * The sample rate of the timer used to increase frequency
   */
#define DEFAULT_TIMER_RATE_MIN (20 * USEC_PER_MSEC)
#define DEFAULT_TIMER_RATE_MID (30 * USEC_PER_MSEC)
#define DEFAULT_TIMER_RATE_MAX (40 * USEC_PER_MSEC)
	unsigned long timer_rate;

  /* Busy SDF parameters*/
#define MIN_BUSY_TIME (100 * USEC_PER_MSEC)

  /*
   * Wait this long before raising speed above hispeed, by default a single
   * timer interval.
   */
  spinlock_t above_hispeed_delay_lock;
  unsigned int *above_hispeed_delay;
  int nabove_hispeed_delay;

  /* Non-zero means indefinite speed boost active */
  int boost;
  /* Duration of a boot pulse in usecs */
  int boostpulse_duration;
  /* End time of boost pulse in ktime converted to usecs */
  u64 boostpulse_endtime;

  /*
   * Max additional time to wait in idle, beyond timer_rate, at speeds above
   * minimum before wakeup to reduce speed, or -1 if unnecessary.
   */
#define DEFAULT_TIMER_SLACK_MIN (4 * DEFAULT_TIMER_RATE_MIN)
#define DEFAULT_TIMER_SLACK_MID (4 * DEFAULT_TIMER_RATE_MID)
#define DEFAULT_TIMER_SLACK_MAX (4 * DEFAULT_TIMER_RATE_MAX)
 	unsigned long timer_slack;

   /*
	  * The maximum frequencies for inactivity, or the time which the
		* the load does not meet the minimum requirements to scale.
		* Devices that support powersuspend will also consider current
		* screen state in determining the maximum frequency values.
	  */
#define DEFAULT_INACTIVE_FREQ_ON_MIN		1209600
#define DEFAULT_INACTIVE_FREQ_ON_MID		1766400
#define DEFAULT_INACTIVE_FREQ_ON_MAX		1900800
#define DEFAULT_INACTIVE_FREQ_OFF_MIN		902400
#define DEFAULT_INACTIVE_FREQ_OFF_MID		1440000
#define DEFAULT_INACTIVE_FREQ_OFF_MAX		1555200
#ifdef CONFIG_HAS_EARLYSUSPEND
	unsigned int max_inactive_freq_screen_on;
	unsigned int max_inactive_freq_screen_off;
#endif
	unsigned int max_inactive_freq;

  bool io_is_busy;

  /*
   * If the max load among other CPUs is higher than up_threshold_any_cpu_load
   * and if the highest frequency among the other CPUs is higher than
   * up_threshold_any_cpu_freq then do not let the frequency to drop below
   * sync_freq
   */
  unsigned int up_threshold_any_cpu_load;
  unsigned int sync_freq;
  unsigned int up_threshold_any_cpu_freq;
};

struct umbrella_core_policy {
	struct cpufreq_policy *policy;
	struct umbrella_core_tunables *tunables;
	struct list_head tunables_hook;
};

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
struct cpufreq_loadinfo {
	unsigned int load;
	unsigned int freq;
	u64 timestamp;
};

static spinlock_t mode_lock;

#define DEFAULT_TARGET_LOAD 80
static unsigned int default_target_loads[] = {
  DEFAULT_TARGET_LOAD
};

/* Default HISPEED DELAY : DEFAULT_SAMPLING_RATE . */
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_TIMER_RATE_MIN
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY
};

/* Iterate over umbrella_core policies for tunables */
#define for_each_ipolicy(__ip)	\
	list_for_each_entry(__ip, &tunables->attr_set.policy_list, tunables_hook)

static struct umbrella_core_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

#define MULTI_MODE	2
#define SINGLE_MODE	1
#define NO_MODE	0

static unsigned int uc_mode = 0;
static unsigned int enforced_mode = 0;
static u64 mode_check_timestamp = 0;

#define DEFAULT_MULTI_ENTER_TIME (4 * DEFAULT_TIMER_RATE_MIN)
static unsigned long multi_enter_time = DEFAULT_MULTI_ENTER_TIME;
static unsigned long time_in_multi_enter = 0;
static unsigned int multi_enter_load = 4 * DEFAULT_TARGET_LOAD;

#define DEFAULT_MULTI_EXIT_TIME (16 * DEFAULT_TIMER_RATE_MIN)
static unsigned long multi_exit_time = DEFAULT_MULTI_EXIT_TIME;
static unsigned long time_in_multi_exit = 0;
static unsigned int multi_exit_load = 4 * DEFAULT_TARGET_LOAD;

#define DEFAULT_SINGLE_ENTER_TIME (8 * DEFAULT_TIMER_RATE_MIN)
static unsigned long single_enter_time = DEFAULT_SINGLE_ENTER_TIME;
static unsigned long time_in_single_enter = 0;
static unsigned int single_enter_load = DEFAULT_TARGET_LOAD;

#define DEFAULT_SINGLE_EXIT_TIME (4 * DEFAULT_TIMER_RATE_MIN)
static unsigned long single_exit_time = DEFAULT_SINGLE_EXIT_TIME;
static unsigned long time_in_single_exit = 0;
static unsigned int single_exit_load = DEFAULT_TARGET_LOAD;

static unsigned int param_index = 0;
static unsigned int cur_param_index = 0;

#define MAX_PARAM_SET 4 /* ((MULTI_MODE | SINGLE_MODE | NO_MODE) + 1) */
static unsigned int hispeed_freq_set[MAX_PARAM_SET];
static unsigned long go_hispeed_load_set[MAX_PARAM_SET];
static unsigned int *target_loads_set[MAX_PARAM_SET];
static int ntarget_loads_set[MAX_PARAM_SET];
static unsigned long min_sample_time_set[MAX_PARAM_SET];
static unsigned long timer_rate_set[MAX_PARAM_SET];
static unsigned int *above_hispeed_delay_set[MAX_PARAM_SET];
static int nabove_hispeed_delay_set[MAX_PARAM_SET];
static unsigned int sampling_down_factor_set[MAX_PARAM_SET];
#endif /* CONFIG_UC_MODE_AUTO_CHANGE */

#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
/* BIMC freq vs BW table
 * BW for 8084 :
 *  762      1144    1525    2288    3051    3952    4684    5859    7019    8056    10101   12145   16250
 * Freq for 8974 (KHz):
 *  19200    37500   50000   75000   100000  150000  200000  307200  460800  614400  825600
 * Freq for 8084 (KHz):
 *  19200    37500   50000   75000   100000  150000  200000  307200  384000  460800  556800  691200  825600  931200
 */
static unsigned long bimc_hispeed_freq = 0	// bimc hispeed freq on mode change. default : MHz
static int mode_count = 0;
extern int request_bimc_clk(unsigned long request_clk);
extern void msm_pm_retention_mode_enable(bool enable);
static void mode_auto_change_boost(struct work_struct *work);
static struct workqueue_struct *mode_auto_change_boost_wq;
static struct work_struct mode_auto_change_boost_work;
#endif

static void cpufreq_umbrella_core_timer_resched(
	struct umbrella_core_cpuinfo *pcpu)
{
  struct umbrella_core_tunables *tunables = pcpu->ipolicy->tunables;
	unsigned long expires;
	unsigned long flags;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
		get_cpu_idle_time(smp_processor_id(),
				  &pcpu->time_in_idle_timestamp, tunables->io_is_busy);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	expires = jiffies + usecs_to_jiffies(tunables->timer_rate);
	mod_timer(&pcpu->cpu_timer, expires);

	if (tunables->timer_slack >= 0 && pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack);
		mod_timer(&pcpu->cpu_slack_timer, expires);
	}

	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The cpu_timer and cpu_slack_timer must be deactivated when calling this
 * function.
 */
static void cpufreq_umbrella_core_timer_start(int cpu, int time_override)
{
	struct umbrella_core_cpuinfo *pcpu = &per_cpu(umbrella_core_cpuinfo, cpu);
  struct umbrella_core_tunables *tunables = pcpu->ipolicy->tunables;
	unsigned long flags;
	unsigned long expires;
	if (time_override)
		expires = jiffies + time_override;
	else
		expires = jiffies + usecs_to_jiffies(tunables->timer_rate);

	pcpu->cpu_timer.expires = expires;
	add_timer_on(&pcpu->cpu_timer, cpu);
	if (tunables->timer_slack >= 0 && pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
		get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp,
				  tunables->io_is_busy);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

static unsigned int
freq_to_above_hispeed_delay(struct umbrella_core_tunables *tunables,
			    unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
	     freq >= tunables->above_hispeed_delay[i + 1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	ret = (ret > (1 * USEC_PER_MSEC)) ? (ret - (1 * USEC_PER_MSEC)) : ret;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	return ret;
}

static unsigned int freq_to_targetload(struct umbrella_core_tunables *tunables,
				       unsigned int freq)
{
	unsigned long flags;
	unsigned int ret;
	int i;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
	     freq >= tunables->target_loads[i + 1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */

static unsigned int choose_freq(struct umbrella_core_cpuinfo *pcpu,
  unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index = 0;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(pcpu->ipolicy->tunables, freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		if (cpufreq_frequency_table_target(
			    pcpu->policy, loadadjfreq / tl,
			    CPUFREQ_RELATION_L))
			break;
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, freqmax - 1,
              CPUFREQ_RELATION_H))
					break;
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, freqmin + 1,
              CPUFREQ_RELATION_L))
					break;
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(int cpu)
{
	struct umbrella_core_cpuinfo *pcpu = &per_cpu(umbrella_core_cpuinfo, cpu);
  struct umbrella_core_tunables *tunables = pcpu->ipolicy->tunables;
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned int cur_load = 0;
	struct cpufreq_loadinfo *cur_loadinfo = &per_cpu(umbrella_core_cpuinfo, cpu);
#endif
	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
	delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	cur_load = (unsigned int)(active_time * 100) / delta_time;
	cur_loadinfo->load = (cur_load * pcpu->policy->cur) /
									pcpu->policy->cpuinfo.max_freq;
	pcpu->policy->load_at_max = cur_loadinfo->load;
	cur_loadinfo->freq = pcpu->policy->cur;
	cur_loadinfo->timestamp = now;
#endif

	return now;
}

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
static unsigned int check_mode(int cpu, unsigned int cur_mode, u64 now)
{
  struct umbrella_core_cpuinfo *pcpu = &per_cpu(umbrella_core_cpuinfo, cpu);
  struct umbrella_core_tunables *tunables = pcpu->ipolicy->tunables;
	int i;
	unsigned int ret=cur_mode, total_load=0, max_single_load=0;
	struct cpufreq_loadinfo *cur_loadinfo;

	if (now - mode_check_timestamp < tunables->timer_rate - 1000)
		return ret;

	if (now - mode_check_timestamp > tunables->timer_rate + 1000)
		mode_check_timestamp = now - tunables->timer_rate;

	for_each_online_cpu(i) {
		cur_loadinfo = &per_cpu(umbrella_core_cpuinfo, i);
		total_load += cur_loadinfo->load;
		if (cur_loadinfo->load > max_single_load)
			max_single_load = cur_loadinfo->load;
	}

	if (!(cur_mode & SINGLE_MODE)) {
		if (max_single_load >= single_enter_load)
			time_in_single_enter += now - mode_check_timestamp;
		else
			time_in_single_enter = 0;

		if (time_in_single_enter >= single_enter_time)
			ret |= SINGLE_MODE;
	}

	if (!(cur_mode & MULTI_MODE)) {
		if (total_load >= multi_enter_load)
			time_in_multi_enter += now - mode_check_timestamp;
		else
			time_in_multi_enter = 0;

		if (time_in_multi_enter >= multi_enter_time)
			ret |= MULTI_MODE;
	}

	if (cur_mode & SINGLE_MODE) {
		if (max_single_load < single_exit_load)
			time_in_single_exit += now - mode_check_timestamp;
		else
			time_in_single_exit = 0;

		if (time_in_single_exit >= single_exit_time)
			ret &= ~SINGLE_MODE;
	}

	if (cur_mode & MULTI_MODE) {
		if (total_load < multi_exit_load)
			time_in_multi_exit += now - mode_check_timestamp;
		else
			time_in_multi_exit = 0;

		if (time_in_multi_exit >= multi_exit_time)
			ret &= ~MULTI_MODE;
	}

	trace_cpufreq_umbrella_core_mode(cpu, total_load,
		time_in_single_enter, time_in_multi_enter,
		time_in_single_exit, time_in_multi_exit, ret);

	if (time_in_single_enter >= single_enter_time)
		time_in_single_enter = 0;
	if (time_in_multi_enter >= multi_enter_time)
		time_in_multi_enter = 0;
	if (time_in_single_exit >= single_exit_time)
		time_in_single_exit = 0;
	if (time_in_multi_exit >= multi_exit_time)
		time_in_multi_exit = 0;
	mode_check_timestamp = now;

	return ret;
}

static void set_new_param_set(unsigned int index,
  struct umbrella_core_tunables *tunables)
{
	unsigned long flags;

	tunables->hispeed_freq = hispeed_freq_set[index];
	tunables->go_hispeed_load = go_hispeed_load_set[index];

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	tunables->target_loads = target_loads_set[index];
	tunables->ntarget_loads =	ntarget_loads_set[index];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);

	tunables->min_sample_time = min_sample_time_set[index];
	tunables->timer_rate = timer_rate_set[index];

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	tunables->above_hispeed_delay = above_hispeed_delay_set[index];
	tunables->nabove_hispeed_delay = nabove_hispeed_delay_set[index];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);

	cur_param_index = index;
}

static void enter_mode(struct umbrella_core_tunables *tunables)
{
#if 1
	set_new_param_set(uc_mode, tunables);
#else
	set_new_param_set(1, tunables);
#endif
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
	queue_work(mode_auto_change_boost_wq, &mode_auto_change_boost_work);
#endif
}

static void exit_mode(struct umbrella_core_tunables *tunables)
{
	set_new_param_set(0, tunables);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
	queue_work(mode_auto_change_boost_wq, &mode_auto_change_boost_work);
#endif
}
#endif

static void cpufreq_umbrella_core_timer(struct timer_list *timers)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
  int cpu = smp_processor_id();
	struct umbrella_core_cpuinfo *pcpu = &per_cpu(umbrella_core_cpuinfo, cpu);
  struct umbrella_core_tunables *tunables = pcpu->ipolicy->tunables;
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	bool boosted;
	unsigned long mod_min_sample_time;
	int i, max_load;
	unsigned int max_freq;
	struct umbrella_core_cpuinfo *picpu;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned int new_mode;
#endif
	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	pcpu->nr_timer_resched = 0;
	spin_lock_irqsave(&pcpu->load_lock, flags);
	now = update_load(cpu);
	delta_time = (unsigned int)(now - pcpu->cputime_speedadj_timestamp);
	cputime_speedadj = pcpu->cputime_speedadj;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		goto rearm;

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags);
	if (enforced_mode)
		new_mode = enforced_mode;
	else
		new_mode = check_mode(cpu, uc_mode, now);
	if (new_mode != uc_mode) {
		uc_mode = new_mode;
		if (new_mode & MULTI_MODE || new_mode & SINGLE_MODE) {
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
			++mode_count;
#endif
			pr_info("Governor: enter mode 0x%x\n", uc_mode);
			enter_mode(tunables);
		} else {
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
			mode_count=0;
#endif
			pr_info("Governor: exit mode 0x%x\n", uc_mode);
			exit_mode(tunables);
		}
	}
	spin_unlock_irqrestore(&mode_lock, flags);
#endif

	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / pcpu->target_freq;
	pcpu->prev_load = cpu_load;
	boosted = tunables->boost || now < tunables->boostpulse_endtime;
	pcpu->policy->util = cpu_load;

	if (cpu_load >= tunables->go_hispeed_load || boosted) {
		if (pcpu->target_freq < tunables->hispeed_freq) {
			new_freq = tunables->hispeed_freq;
		} else {
			new_freq = choose_freq(pcpu, loadadjfreq);

			if (new_freq < tunables->hispeed_freq)
				new_freq = tunables->hispeed_freq;
		}
		if (new_freq > tunables->max_inactive_freq
      && cpu_load < default_target_loads[cpu])
			new_freq = tunables->max_inactive_freq;
	} else {
		new_freq = choose_freq(pcpu, loadadjfreq);

		if (tunables->sync_freq && new_freq < tunables->sync_freq) {

			max_load = 0;
			max_freq = 0;

			for_each_online_cpu(i) {
				picpu = &per_cpu(umbrella_core_cpuinfo, i);

				if (i == cpu || picpu->prev_load <
						tunables->up_threshold_any_cpu_load)
					continue;

				max_load = max(max_load, picpu->prev_load);
				max_freq = max(max_freq, picpu->target_freq);
			}

			if (max_freq > tunables->up_threshold_any_cpu_freq &&
				max_load >= tunables->up_threshold_any_cpu_load)
				new_freq = tunables->sync_freq;
		}
	}

	if (pcpu->target_freq >= tunables->hispeed_freq &&
	    new_freq > pcpu->target_freq &&
	    now - pcpu->hispeed_validate_time <
	    freq_to_above_hispeed_delay(tunables, pcpu->target_freq)) {
		trace_cpufreq_umbrella_core_notyet(
			cpu, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		goto rearm;
	}

	pcpu->hispeed_validate_time = now;

	if (cpufreq_frequency_table_target(pcpu->policy,
					   new_freq, CPUFREQ_RELATION_L))
		goto rearm;

	new_freq = pcpu->freq_table[index].frequency;

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (tunables->sampling_down_factor && pcpu->policy->cur == pcpu->policy->max)
		mod_min_sample_time = tunables->sampling_down_factor;
	else
		mod_min_sample_time = tunables->min_sample_time;

	if (pcpu->limits_changed) {
		if (tunables->sampling_down_factor &&
			(pcpu->policy->cur != pcpu->policy->max))
			mod_min_sample_time = 0;

		pcpu->limits_changed = false;
	}

	if (new_freq < pcpu->floor_freq) {
		if (now - pcpu->floor_validate_time < mod_min_sample_time) {
			trace_cpufreq_umbrella_core_notyet(
				cpu, cpu_load, pcpu->target_freq,
				pcpu->policy->cur, new_freq);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to hispeed_freq.  If boosted to hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (!boosted || new_freq > tunables->hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
	}

    if (pcpu->target_freq == new_freq &&
        pcpu->target_freq <= pcpu->policy->cur) {
		trace_cpufreq_umbrella_core_already(
			cpu, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		goto rearm_if_notmax;
	}

	trace_cpufreq_umbrella_core_target(cpu, cpu_load, pcpu->target_freq,
					 pcpu->policy->cur, new_freq);

	pcpu->target_freq = new_freq;
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(cpu, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process(speedchange_task);

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
		goto rearm;
#else
		goto exit;
#endif

rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_umbrella_core_timer_resched(pcpu);

exit:
	up_read(&pcpu->enable_sem);
	return;
}

static void cpufreq_umbrella_core_idle_start(void)
{
	struct umbrella_core_cpuinfo *pcpu =
		&per_cpu(umbrella_core_cpuinfo, smp_processor_id());
	int pending;
	u64 now;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	pending = timer_pending(&pcpu->cpu_timer);

	if (pcpu->target_freq != pcpu->policy->min) {
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending) {
			cpufreq_umbrella_core_timer_resched(pcpu);

			now = ktime_to_us(ktime_get());
			if ((pcpu->policy->cur == pcpu->policy->max) &&
				(now - pcpu->hispeed_validate_time) >
							MIN_BUSY_TIME) {
				pcpu->floor_validate_time = now;
			}

		}
	}

	up_read(&pcpu->enable_sem);
}

static void cpufreq_umbrella_core_idle_end(void)
{
	struct umbrella_core_cpuinfo *pcpu =
		&per_cpu(umbrella_core_cpuinfo, smp_processor_id());

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	/* Arm the timer for 1-2 ticks later if not already. */
	if (!timer_pending(&pcpu->cpu_timer)) {
		cpufreq_umbrella_core_timer_resched(pcpu);
	} else if (time_after_eq(jiffies, pcpu->cpu_timer.expires)) {
		del_timer(&pcpu->cpu_timer);
		del_timer(&pcpu->cpu_slack_timer);
		cpufreq_umbrella_core_timer(smp_processor_id());
	}

	up_read(&pcpu->enable_sem);
}

static int cpufreq_umbrella_core_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct umbrella_core_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;

			pcpu = &per_cpu(umbrella_core_cpuinfo, cpu);
			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				struct umbrella_core_cpuinfo *pjcpu =
					&per_cpu(umbrella_core_cpuinfo, j);

				if (pjcpu->target_freq > max_freq)
					max_freq = pjcpu->target_freq;
			}

			if (max_freq != pcpu->policy->cur)
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
			trace_cpufreq_umbrella_core_setspeed(cpu,
						     pcpu->target_freq,
						     pcpu->policy->cur);

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

static void cpufreq_umbrella_core_boost(struct umbrella_core_tunables *tunables)
{
	int i;
	int anyboost = 0;
	unsigned long flags;
	struct umbrella_core_cpuinfo *pcpu;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags);

	for_each_online_cpu(i) {
		pcpu = &per_cpu(umbrella_core_cpuinfo, i);

		if (pcpu->target_freq < tunables->hispeed_freq) {
			pcpu->target_freq = tunables->hispeed_freq;
			cpumask_set_cpu(i, &speedchange_cpumask);
			pcpu->hispeed_validate_time =
				ktime_to_us(ktime_get());
			anyboost = 1;
		}

		/*
		 * Set floor freq and (re)start timer for when last
		 * validated.
		 */

		pcpu->floor_freq = tunables->hispeed_freq;
		pcpu->floor_validate_time = ktime_to_us(ktime_get());
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	if (anyboost)
		wake_up_process(speedchange_task);
}

static int cpufreq_umbrella_core_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct umbrella_core_cpuinfo *pcpu;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_POSTCHANGE) {
		pcpu = &per_cpu(umbrella_core_cpuinfo, freq->policy->cpu);
		if (!down_read_trylock(&pcpu->enable_sem))
			return 0;
		if (!pcpu->governor_enabled) {
			up_read(&pcpu->enable_sem);
			return 0;
		}

		for_each_cpu(cpu, pcpu->policy->cpus) {
			struct umbrella_core_cpuinfo *pjcpu =
				&per_cpu(umbrella_core_cpuinfo, cpu);
			if (cpu != freq->policy->cpu) {
				if (!down_read_trylock(&pjcpu->enable_sem))
					continue;
				if (!pjcpu->governor_enabled) {
					up_read(&pjcpu->enable_sem);
					continue;
				}
			}
			spin_lock_irqsave(&pjcpu->load_lock, flags);
			update_load(cpu);
			spin_unlock_irqrestore(&pjcpu->load_lock, flags);
			if (cpu != freq->policy->cpu)
				up_read(&pjcpu->enable_sem);
		}

		up_read(&pcpu->enable_sem);
	}
	return 0;
}

struct notifier_block cpufreq_umbrella_core_idle_nb = {
	.notifier_call = cpufreq_umbrella_core_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static struct umbrella_core_tunables *to_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct umbrella_core_tunables, attr_set);
}

#define show_one(file_name, type)					\
static ssize_t show_##file_name(struct gov_attr_set *attr_set, char *buf) \
{									\
	struct umbrella_core_tunables *tunables = to_tunables(attr_set);	\
	return sprintf(buf, type "\n", tunables->file_name);		\
}

static ssize_t show_target_loads(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	for (i = 0; i < ntarget_loads_set[param_index]; i++)
		ret += sprintf(buf + ret, "%u%s", target_loads_set[param_index][i],
			       i & 0x1 ? ":" : " ");
#else
	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");
#endif
	ret += sprintf(buf + --ret, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

static ssize_t store_target_loads(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ntokens;
	unsigned int *new_target_loads = NULL;
	unsigned long flags;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_RET(new_target_loads);

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
#endif
	spin_lock_irqsave(&tunables->target_loads_lock, flags);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	if (target_loads_set[param_index] != default_target_loads)
		kfree(target_loads_set[param_index]);
	target_loads_set[param_index] = new_target_loads;
	ntarget_loads_set[param_index] = ntokens;
	if (cur_param_index == param_index) {
		tunables->target_loads = new_target_loads;
		tunables->ntarget_loads = ntokens;
	}
#else
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
  tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
#endif
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_unlock_irqrestore(&mode_lock, flags2);
#endif
	return count;
}

gov_attr_rw(target_loads);

static ssize_t show_above_hispeed_delay(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	for (i = 0; i < nabove_hispeed_delay_set[param_index]; i++)
		ret += sprintf(buf + ret, "%u%s", above_hispeed_delay_set[param_index][i],
			       i & 0x1 ? ":" : " ");
#else
	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");
#endif
	ret += sprintf(buf + --ret, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static ssize_t store_above_hispeed_delay(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ntokens;
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_RET(new_above_hispeed_delay);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
#endif
	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	if (above_hispeed_delay_set[param_index] != default_above_hispeed_delay)
		kfree(above_hispeed_delay_set[param_index]);
	above_hispeed_delay_set[param_index] = new_above_hispeed_delay;
	nabove_hispeed_delay_set[param_index] = ntokens;
	if (cur_param_index == param_index) {
		tunables->above_hispeed_delay = new_above_hispeed_delay;
		tunables->nabove_hispeed_delay = ntokens;
	}
#else
	if (above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
#endif
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_unlock_irqrestore(&mode_lock, flags2);
#endif
	return count;

}

gov_attr_rw(above_hispeed_delay);

static ssize_t show_hispeed_freq(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	return sprintf(buf, "%u\n", hispeed_freq_set[param_index]);
#else
	return sprintf(buf, "%u\n", tunables->hispeed_freq);
#endif
}

static ssize_t store_hispeed_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	long unsigned int val;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
	hispeed_freq_set[param_index] = val;
	if (cur_param_index == param_index)
		tunables->hispeed_freq = val;
	spin_unlock_irqrestore(&mode_lock, flags2);
#else
	tunables->hispeed_freq = val;
#endif
	return count;
}

gov_attr_rw(hispeed_freq);

static ssize_t show_sampling_down_factor(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	return sprintf(buf, "%u\n", sampling_down_factor_set[param_index]);
#else
	return sprintf(buf, "%u\n", tunables->sampling_down_factor);
#endif
}

static ssize_t store_sampling_down_factor(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	long unsigned int val;

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
	sampling_down_factor_set[param_index] = val;
	if (cur_param_index == param_index)
		tunables->sampling_down_factor = val;
	spin_unlock_irqrestore(&mode_lock, flags2);
#else
	tunables->sampling_down_factor = val;
#endif
	return count;
}

gov_attr_rw(sampling_down_factor);

static ssize_t show_go_hispeed_load(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	return sprintf(buf, "%lu\n", go_hispeed_load_set[param_index]);
#else
	return sprintf(buf, "%lu\n", tunables->go_hispeed_load);
#endif
}

static ssize_t store_go_hispeed_load(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
	go_hispeed_load_set[param_index] = val;
	if (cur_param_index == param_index)
		tunables->go_hispeed_load = val;
	spin_unlock_irqrestore(&mode_lock, flags2);
#else
	tunables->go_hispeed_load = val;
#endif
	return count;
}

gov_attr_rw(go_hispeed_load);

static ssize_t show_min_sample_time_attr(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	return sprintf(buf, "%lu\n", min_sample_time_set[param_index]);
#else
	return sprintf(buf, "%lu\n", tunables->min_sample_time);
#endif
}

static ssize_t store_min_sample_time_attr(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
	min_sample_time_set[param_index] = val;
	if (cur_param_index == param_index)
		tunables->min_sample_time = val;
	spin_unlock_irqrestore(&mode_lock, flags2);
#else
	tunables->min_sample_time = val;
#endif
	return count;
}

gov_attr_rw(min_sample_time_attr);

static ssize_t show_timer_rate(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	return sprintf(buf, "%lu\n", timer_rate_set[param_index]);
#else
	return sprintf(buf, "%lu\n", tunables->timer_rate);
#endif
}

static ssize_t store_timer_rate(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	unsigned long flags2;
#endif
	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_irqsave(&mode_lock, flags2);
	timer_rate_set[param_index] = val;
	if (cur_param_index == param_index)
		tunables->timer_rate = val;
	spin_unlock_irqrestore(&mode_lock, flags2);
#else
	tunables->timer_rate = val;
#endif
	return count;
}

gov_attr_rw(timer_rate);

static ssize_t store_timer_slack(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	tunables->timer_slack = val;
	return count;
}

show_one(timer_slack, "%lu");
gov_attr_rw(timer_slack);

static ssize_t store_boost(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boost = val;

	if (tunables->boost) {
		trace_cpufreq_umbrella_core_boost("on");
		cpufreq_umbrella_core_boost(tunables);
	} else {
		trace_cpufreq_umbrella_core_unboost("off");
	}

	return count;
}

show_one(boost, "%u");
gov_attr_rw(boost);

static ssize_t store_boostpulse(struct gov_attr_set *attr_set, const char *buf,
				size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_endtime = ktime_to_us(ktime_get()) + tunables->boostpulse_duration;
	trace_cpufreq_umbrella_core_boost("pulse");
	cpufreq_umbrella_core_boost(tunables);
	return count;
}

gov_attr_wo(boostpulse);

static ssize_t store_boostpulse_duration(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_duration = val;
	return count;
}

show_one(boostpulse_duration, "%u");
gov_attr_rw(boostpulse_duration);

static ssize_t store_io_is_busy(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->io_is_busy = val;
	return count;
}

show_one(io_is_busy, "%u");
gov_attr_rw(io_is_busy);

#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
static ssize_t show_bimc_hispeed_freq_attr(struct gov_attr_set *attr_set, char *buf)
{
  return sprintf(buf, "%lu\n", bimc_hispeed_freq);
}

static ssize_t store_bimc_hispeed_freq_attr(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	bimc_hispeed_freq = val;
	pr_info("cpufreq-umbrella_core: bimc_hispeed_freq will be set to : (input)%lu \n", bimc_hispeed_freq);

	return count;
}

gov_attr_rw(bimc_hispeed_freq_attr);
#endif	// CONFIG_UC_MODE_AUTO_CHANGE_BOOST

static ssize_t store_sync_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->sync_freq = val;
	return count;
}

show_one(sync_freq, "%u");
gov_attr_rw(sync_freq);

#ifdef CONFIG_HAS_EARLYSUSPEND
static ssize_t store_max_inactive_freq_screen_on(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
    unsigned int new_max_inactive_freq_screen_on;

    if (!sscanf(buf, "%du", &new_max_inactive_freq_screen_on))
        return -EINVAL;

    if (new_max_inactive_freq_screen_on == tunables->max_inactive_freq_screen_on)
        return count;

    tunables->max_inactive_freq_screen_on = new_max_inactive_freq_screen_on;
    if (tunables->max_inactive_freq_screen_on != tunables->max_inactive_freq) {
        tunables->max_inactive_freq = tunables->max_inactive_freq_screen_on;
    }
    return count;
}

show_one(max_inactive_freq_screen_on, "%d");
gov_attr_rw(max_inactive_freq_screen_on);

static ssize_t store_max_inactive_freq_screen_off(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
    unsigned int new_max_inactive_freq_screen_off;

    if (!sscanf(buf, "%du", &new_max_inactive_freq_screen_off))
        return -EINVAL;

    if (new_max_inactive_freq_screen_off == tunables->max_inactive_freq_screen_off)
        return count;

    tunables->max_inactive_freq_screen_off = new_max_inactive_freq_screen_off;
    return count;
}

show_one(max_inactive_freq_screen_off, "%d");
gov_attr_rw(max_inactive_freq_screen_off);
#endif

static ssize_t store_max_inactive_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
    unsigned int new_max_inactive_freq;

    if (!sscanf(buf, "%du", &new_max_inactive_freq))
        return -EINVAL;

    if (new_max_inactive_freq == tunables->max_inactive_freq)
        return count;

    tunables->max_inactive_freq = new_max_inactive_freq;
    return count;
}

show_one(max_inactive_freq, "%d");
gov_attr_rw(max_inactive_freq);

static ssize_t show_up_threshold_any_cpu_load(struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	return snprintf(buf, PAGE_SIZE, "%u\n", tunables->up_threshold_any_cpu_load);
}

static ssize_t store_up_threshold_any_cpu_load(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->up_threshold_any_cpu_load = val;
	return count;
}

gov_attr_rw(up_threshold_any_cpu_load);

static ssize_t show_up_threshold_any_cpu_freq(
  struct gov_attr_set *attr_set, char *buf)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	return snprintf(buf, PAGE_SIZE, "%u\n", tunables->up_threshold_any_cpu_freq);
}

static ssize_t store_up_threshold_any_cpu_freq(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
  struct umbrella_core_tunables *tunables = to_tunables(attr_set);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->up_threshold_any_cpu_freq = val;
	return count;
}

gov_attr_rw(up_threshold_any_cpu_freq);

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
#define index(obj_name, obj_attr)					\
static ssize_t show_##obj_name(struct gov_attr_set *attr_set, char *buf)				\
{				\
        return sprintf(buf, "%u\n", obj_name);				\
}									\
									\
static ssize_t store_##obj_name(struct gov_attr_set *attr_set,							\
				  const char *buf, size_t count)							\
{							\
        int ret;							\
        long unsigned int val;						\
									\
        ret = kstrtoul(buf, 0, &val);				\
        if (ret < 0)							\
                return ret;						\
									\
	val &= MULTI_MODE | SINGLE_MODE | NO_MODE;			\
        obj_name = val;							\
        return count;							\
}									\
									\
static struct governor_attr obj_attr = __ATTR(obj_name, 0644,		\
                show_##obj_name, store_##obj_name);			\

index(uc_mode, uc_mode_attr);
index(enforced_mode, enforced_mode_attr);
index(param_index, param_index_attr);

#define load(obj_name, obj_attr)					\
static ssize_t show_##obj_name(struct gov_attr_set *attr_set, char *buf)				\
{				\
        return sprintf(buf, "%u\n", obj_name);				\
}									\
									\
static ssize_t store_##obj_name(struct gov_attr_set *attr_set,							\
				  const char *buf, size_t count)							\
{							\
        int ret;							\
        long unsigned int val;						\
									\
        ret = kstrtoul(buf, 0, &val);				\
        if (ret < 0)							\
                return ret;						\
									\
        obj_name = val;							\
        return count;							\
}									\
									\
static struct governor_attr obj_attr = __ATTR(obj_name, 0644,		\
                show_##obj_name, store_##obj_name);			\

load(multi_enter_load, multi_enter_load_attr);
load(multi_exit_load, multi_exit_load_attr);
load(single_enter_load, single_enter_load_attr);
load(single_exit_load, single_exit_load_attr);

#define time(obj_name, obj_attr)					\
static ssize_t show_##obj_name(struct gov_attr_set *attr_set, char *buf)				\
{				\
        return sprintf(buf, "%lu\n", obj_name);				\
}									\
									\
static ssize_t store_##obj_name(struct gov_attr_set *attr_set,							\
				  const char *buf, size_t count)							\
{							\
        int ret;							\
        unsigned long val;						\
									\
        ret = kstrtoul(buf, 0, &val);				\
        if (ret < 0)							\
                return ret;						\
									\
        obj_name = val;							\
        return count;							\
}									\
									\
static struct governor_attr obj_attr = __ATTR(obj_name, 0644,		\
                show_##obj_name, store_##obj_name);			\

time(multi_enter_time, multi_enter_time_attr);
time(multi_exit_time, multi_exit_time_attr);
time(single_enter_time, single_enter_time_attr);
time(single_exit_time, single_exit_time_attr);
#endif

static struct attribute *umbrella_core_attributes[] = {
	&target_loads.attr,
	&above_hispeed_delay.attr,
	&hispeed_freq.attr,
	&go_hispeed_load.attr,
	&min_sample_time_attr.attr,
	&timer_rate.attr,
	&timer_slack.attr,
	&boost.attr,
	&boostpulse.attr,
	&boostpulse_duration.attr,
	&io_is_busy.attr,
	&sampling_down_factor.attr,
	&sync_freq.attr,
	&up_threshold_any_cpu_load.attr,
	&up_threshold_any_cpu_freq.attr,
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	&uc_mode_attr.attr,
	&enforced_mode_attr.attr,
	&param_index_attr.attr,
	&multi_enter_load_attr.attr,
	&multi_exit_load_attr.attr,
	&single_enter_load_attr.attr,
	&single_exit_load_attr.attr,
	&multi_enter_time_attr.attr,
	&multi_exit_time_attr.attr,
	&single_enter_time_attr.attr,
	&single_exit_time_attr.attr,
#endif
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
	&bimc_hispeed_freq_attr.attr,
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
    &max_inactive_freq_screen_on.attr,
    &max_inactive_freq_screen_off.attr,
#endif
    &max_inactive_freq.attr,
	NULL,
};

static struct kobj_type umbrella_core_tunables_ktype = {
	.default_attrs = umbrella_core_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

static int cpufreq_umbrella_core_idle_notifier(struct notifier_block *nb,
					     unsigned long val, void *data)
{
	switch (val) {
	case IDLE_START:
		cpufreq_umbrella_core_idle_start();
		break;
	case IDLE_END:
		cpufreq_umbrella_core_idle_end();
		break;
	}

	return 0;
}

/* umbrella_core Governor callbacks */
struct umbrella_core_governor {
	struct cpufreq_governor gov;
	unsigned int usage_count;
};

static struct umbrella_core_governor umbrella_core_gov;

#define CPU_FREQ_GOV_UMBRELLA_CORE	(&umbrella_core_gov.gov)

static struct umbrella_core_policy *
umbrella_core_policy_alloc(struct cpufreq_policy *policy)
{
	struct umbrella_core_policy *ipolicy;

	ipolicy = kzalloc(sizeof(*ipolicy), GFP_KERNEL);
	if (!ipolicy)
		return NULL;

	ipolicy->policy = policy;

	return ipolicy;
}

static void umbrella_core_policy_free(struct umbrella_core_policy *ipolicy)
{
	kfree(ipolicy);
}

static struct umbrella_core_tunables *
umbrella_core_tunables_alloc(struct umbrella_core_policy *ipolicy)
{
	struct umbrella_core_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return NULL;

	gov_attr_set_init(&tunables->attr_set, &ipolicy->tunables_hook);
	if (!have_governor_per_policy())
		global_tunables = tunables;

	ipolicy->tunables = tunables;

	return tunables;
}

static void umbrella_core_tunables_free(struct umbrella_core_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
void cpufreq_param_set_init(struct umbrella_core_tunables *tunables)
{
	unsigned int i;
	unsigned long flags;

	multi_enter_load = DEFAULT_TARGET_LOAD * num_possible_cpus();

	spin_lock_irqsave(&mode_lock, flags);
	for (i=0 ; i<MAX_PARAM_SET; i++) {
		hispeed_freq_set[i] = 0;
		go_hispeed_load_set[i] = tunables->go_hispeed_load;
		target_loads_set[i] = tunables->target_loads;
		ntarget_loads_set[i] = tunables->ntarget_loads;
		min_sample_time_set[i] = tunables->min_sample_time;
		timer_rate_set[i] = tunables->timer_rate;
		above_hispeed_delay_set[i] = tunables->above_hispeed_delay;
		nabove_hispeed_delay_set[i] = tunables->nabove_hispeed_delay;
		sampling_down_factor_set[i] = tunables->sampling_down_factor;
	}
	spin_unlock_irqrestore(&mode_lock, flags);
}
#endif

int cpufreq_umbrella_core_init(struct cpufreq_policy *policy)
{
	struct umbrella_core_policy *ipolicy;
	struct umbrella_core_tunables *tunables;
	int ret;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	ipolicy = umbrella_core_policy_alloc(policy);
	if (!ipolicy)
		return -ENOMEM;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_int_policy;
		}

		policy->governor_data = ipolicy;
		ipolicy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set,
				 &ipolicy->tunables_hook);
		goto out;
	}

	tunables = umbrella_core_tunables_alloc(ipolicy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_int_policy;
	}

	/* Set tunables by cluster - XDA@nalas */
	/*  Fixed and updated by AbandonedCart */
	if (policy->cpu == 0) {
		tunables->hispeed_freq = policy->max;
		tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD_MIN;
		tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME_MIN;
		tunables->boostpulse_duration = DEFAULT_MIN_SAMPLE_TIME_MIN;
		tunables->timer_rate = DEFAULT_TIMER_RATE_MIN;
		tunables->timer_slack = DEFAULT_TIMER_SLACK_MIN;
#ifdef CONFIG_HAS_EARLYSUSPEND
		tunables->max_inactive_freq_screen_on = DEFAULT_INACTIVE_FREQ_ON_MIN;
		tunables->max_inactive_freq_screen_off = DEFAULT_INACTIVE_FREQ_OFF_MIN;
#endif
		tunables->max_inactive_freq = DEFAULT_INACTIVE_FREQ_ON_MIN;
	}
	if (policy->cpu == 4) {
		tunables->hispeed_freq = policy->max;
		tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD_MID;
		tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME_MID;
		tunables->boostpulse_duration = DEFAULT_MIN_SAMPLE_TIME_MID;
		tunables->timer_rate = DEFAULT_TIMER_RATE_MID;
		tunables->timer_slack = DEFAULT_TIMER_SLACK_MID;
#ifdef CONFIG_HAS_EARLYSUSPEND
		tunables->max_inactive_freq_screen_on = DEFAULT_INACTIVE_FREQ_ON_MID;
		tunables->max_inactive_freq_screen_off = DEFAULT_INACTIVE_FREQ_OFF_MID;
#endif
		tunables->max_inactive_freq = DEFAULT_INACTIVE_FREQ_ON_MID;
	}
	if (policy->cpu == 7) {
		tunables->hispeed_freq = policy->max;
		tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD_MAX;
		tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME_MAX;
		tunables->boostpulse_duration = DEFAULT_MIN_SAMPLE_TIME_MAX;
		tunables->timer_rate = DEFAULT_TIMER_RATE_MAX;
		tunables->timer_slack = DEFAULT_TIMER_SLACK_MAX;
#ifdef CONFIG_HAS_EARLYSUSPEND
		tunables->max_inactive_freq_screen_on = DEFAULT_INACTIVE_FREQ_ON_MAX;
		tunables->max_inactive_freq_screen_off = DEFAULT_INACTIVE_FREQ_OFF_MAX;
#endif
		tunables->max_inactive_freq = DEFAULT_INACTIVE_FREQ_ON_MAX;
	}

	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
	ARRAY_SIZE(default_above_hispeed_delay);
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

#ifdef CONFIG_UC_MODE_AUTO_CHANGE
  	spin_lock_init(&mode_lock);
  	cpufreq_param_set_init(tunables);
#endif

	policy->governor_data = ipolicy;

	ret = kobject_init_and_add(&tunables->attr_set.kobj,
				   &umbrella_core_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   umbrella_core_gov.gov.name);
	if (ret)
		goto fail;

	/* One time initialization for governor */
	if (!umbrella_core_gov.usage_count++) {
		idle_notifier_register(&cpufreq_umbrella_core_idle_nb);
		cpufreq_register_notifier(&cpufreq_umbrella_core_idle_nb,
					  CPUFREQ_TRANSITION_NOTIFIER);
	}

 out:
	mutex_unlock(&global_tunables_lock);
	return 0;

 fail:
	policy->governor_data = NULL;
	umbrella_core_tunables_free(tunables);

 free_int_policy:
	mutex_unlock(&global_tunables_lock);

	umbrella_core_policy_free(ipolicy);
	pr_err("governor initialization failed (%d)\n", ret);

	return ret;
}

void cpufreq_umbrella_core_exit(struct cpufreq_policy *policy)
{
	struct umbrella_core_policy *ipolicy = policy->governor_data;
	struct umbrella_core_tunables *tunables = ipolicy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	/* Last policy using the governor ? */
	if (!--umbrella_core_gov.usage_count) {
		cpufreq_unregister_notifier(&cpufreq_umbrella_core_idle_nb,
					    CPUFREQ_TRANSITION_NOTIFIER);
		idle_notifier_unregister(&cpufreq_umbrella_core_idle_nb);
	}

	count = gov_attr_set_put(&tunables->attr_set, &ipolicy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		umbrella_core_tunables_free(tunables);

	mutex_unlock(&global_tunables_lock);

	umbrella_core_policy_free(ipolicy);
}

int cpufreq_umbrella_core_start(struct cpufreq_policy *policy)
{
  struct umbrella_core_policy *ipolicy = policy->governor_data;
	struct umbrella_core_tunables *tunables = ipolicy->tunables;
  int rc;
  unsigned int j;
  struct umbrella_core_cpuinfo *pcpu;
  struct cpufreq_frequency_table *freq_table;

  mutex_lock(&gov_lock);

  freq_table = cpufreq_frequency_get_table(policy->cpu);
  if (!tunables->hispeed_freq)
    tunables->hispeed_freq = policy->max;
  #ifdef CONFIG_UC_MODE_AUTO_CHANGE
  for (j=0 ; j<MAX_PARAM_SET ; j++)
    if (!hispeed_freq_set[j])
      hispeed_freq_set[j] = policy->max;
  #endif
  for_each_cpu(j, policy->cpus) {
    pcpu = &per_cpu(umbrella_core_cpuinfo, j);
    pcpu->policy = policy;
    pcpu->target_freq = policy->cur;
    pcpu->freq_table = freq_table;
    pcpu->floor_freq = pcpu->target_freq;
    pcpu->floor_validate_time =
      ktime_to_us(ktime_get());
    pcpu->hispeed_validate_time =
      pcpu->floor_validate_time;
    down_write(&pcpu->enable_sem);
    del_timer_sync(&pcpu->cpu_timer);
    del_timer_sync(&pcpu->cpu_slack_timer);
    cpufreq_umbrella_core_timer_start(j, 0);
    pcpu->governor_enabled = 1;
    up_write(&pcpu->enable_sem);
  }

  /*
   * Do not register the idle hook and create sysfs
   * entries if we have already done so.
   */
  if (++active_count > 1) {
    mutex_unlock(&gov_lock);
    return 0;
  }

  idle_notifier_register(&cpufreq_umbrella_core_idle_nb);
  cpufreq_register_notifier(
    &cpufreq_umbrella_core_idle_nb, CPUFREQ_TRANSITION_NOTIFIER);
  mutex_unlock(&gov_lock);
}

void cpufreq_umbrella_core_stop(struct cpufreq_policy *policy)
{
  unsigned int j;
  struct umbrella_core_cpuinfo *pcpu;
  struct cpufreq_frequency_table *freq_table;

  mutex_lock(&gov_lock);
  for_each_cpu(j, policy->cpus) {
    pcpu = &per_cpu(umbrella_core_cpuinfo, j);
    down_write(&pcpu->enable_sem);
    pcpu->governor_enabled = 0;
    pcpu->target_freq = 0;
    del_timer_sync(&pcpu->cpu_timer);
    del_timer_sync(&pcpu->cpu_slack_timer);
    up_write(&pcpu->enable_sem);
  }

  if (--active_count > 0) {
    mutex_unlock(&gov_lock);
    return;
  }

  cpufreq_unregister_notifier(
    &cpufreq_umbrella_core_idle_nb, CPUFREQ_TRANSITION_NOTIFIER);
  idle_notifier_unregister(&cpufreq_umbrella_core_idle_nb);
  mutex_unlock(&gov_lock);
}

void cpufreq_umbrella_core_limits(struct cpufreq_policy *policy)
{
  struct umbrella_core_policy *ipolicy = policy->governor_data;
	struct umbrella_core_tunables *tunables = ipolicy->tunables;
  unsigned int j;
  struct umbrella_core_cpuinfo *pcpu;
  struct cpufreq_frequency_table *freq_table;
  unsigned long expire_time;

  if (policy->max < policy->cur)
    __cpufreq_driver_target(policy,
        policy->max, CPUFREQ_RELATION_H);
  else if (policy->min > policy->cur)
    __cpufreq_driver_target(policy,
        policy->min, CPUFREQ_RELATION_L);
  for_each_cpu(j, policy->cpus) {
    pcpu = &per_cpu(umbrella_core_cpuinfo, j);

    /* hold write semaphore to avoid race */
    down_write(&pcpu->enable_sem);
    if (pcpu->governor_enabled == 0) {
      up_write(&pcpu->enable_sem);
      continue;
    }

    /* update target_freq firstly */
    if (policy->max < pcpu->target_freq)
      pcpu->target_freq = policy->max;
    /*
     * Delete and reschedule timer.
     * Else the timer callback may return without
     * re-arming the timer when it fails to acquire
     * the semaphore. This race condition may cause the
     * timer to stop unexpectedly.
     */
    del_timer_sync(&pcpu->cpu_timer);
    del_timer_sync(&pcpu->cpu_slack_timer);

    if (pcpu->nr_timer_resched) {
      if (pcpu->policy->max < pcpu->target_freq)
        pcpu->target_freq = pcpu->policy->max;
      if (pcpu->policy->min >= pcpu->target_freq)
        pcpu->target_freq = pcpu->policy->min;
      /*
       * To avoid deferring load evaluation for a
       * long time rearm the timer for the same jiffy
       * as it was supposed to fire at, if it has
       * already been rescheduled once. The timer
       * start and rescheduling functions aren't used
       * here so that the timestamps used for load
       * calculations do not get reset.
       */
      add_timer_on(&pcpu->cpu_timer, j);
      if (tunables->timer_slack >= 0 && pcpu->target_freq >
            pcpu->policy->min)
        add_timer_on(&pcpu->cpu_slack_timer, j);
    } else if (policy->min >= pcpu->target_freq) {
      pcpu->target_freq = policy->min;
      /*
       * Reschedule timer.
       * The governor needs more time to evaluate
       * the load after changing policy parameters.
       */
      cpufreq_umbrella_core_timer_start(j, 0);
      pcpu->nr_timer_resched++;
    } else {
      /*
       * Reschedule timer with variable duration.
       * No boost was applied so the governor
       * doesn't need extra time to evaluate load.
       * The timer can be set to fire quicker if it
       * was already going to expire soon.
       */
      expire_time = pcpu->cpu_timer.expires - jiffies;
      expire_time = min(usecs_to_jiffies(tunables->timer_rate),
            expire_time);
      expire_time = max(MIN_TIMER_JIFFIES,
            expire_time);

      cpufreq_umbrella_core_timer_start(j, expire_time);
      pcpu->nr_timer_resched++;
    }
    pcpu->limits_changed = true;
    up_write(&pcpu->enable_sem);
  }
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void cpufreq_umbrella_core_power_suspend(struct early_suspend *h)
{
    mutex_lock(&gov_lock);
    if (tunables->max_inactive_freq_screen_off != tunables->max_inactive_freq) {
        tunables->max_inactive_freq = tunables->max_inactive_freq_screen_off;
    }
    mutex_unlock(&gov_lock);
}

static void cpufreq_umbrella_core_power_resume(struct early_suspend *h)
{
    mutex_lock(&gov_lock);
    if (tunables->max_inactive_freq_screen_on != tunables->max_inactive_freq) {
        tunables->max_inactive_freq = tunables->max_inactive_freq_screen_on;
    }
    mutex_unlock(&gov_lock);
}

static struct early_suspend cpufreq_umbrella_core_power_suspend_info = {
    .level		= EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
    .suspend  = cpufreq_umbrella_core_power_suspend,
    .resume   = cpufreq_umbrella_core_power_resume,
};
#endif

static void cpufreq_umbrella_core_nop_timer(struct timer_list *timers)
{
	/*
	 * The purpose of slack-timer is to wake up the CPU from IDLE, in order
	 * to decrease its frequency if it is not set to minimum already.
	 *
	 * This is important for platforms where CPU with higher frequencies
	 * consume higher power even at IDLE.
	 */
}

static int __init cpufreq_umbrella_core_gov_init(void)
{
	unsigned int i;
	struct umbrella_core_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(umbrella_core_cpuinfo, i);
    timer_setup(&pcpu->cpu_timer, cpufreq_umbrella_core_timer, TIMER_DEFERRABLE);
    timer_setup(&pcpu->cpu_slack_timer, cpufreq_umbrella_core_nop_timer, 0UL);
		spin_lock_init(&pcpu->load_lock);
		init_rwsem(&pcpu->enable_sem);
	}

	spin_lock_init(&speedchange_cpumask_lock);
#ifdef CONFIG_UC_MODE_AUTO_CHANGE
	spin_lock_init(&mode_lock);
#endif
#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
	mode_auto_change_boost_wq = alloc_workqueue("mode_auto_change_boost_wq", WQ_HIGHPRI, 0);
	if(!mode_auto_change_boost_wq)
		pr_info("mode auto change boost workqueue init error\n");
	INIT_WORK(&mode_auto_change_boost_work, mode_auto_change_boost);
#endif
	mutex_init(&gov_lock);
	speedchange_task =
		kthread_create(cpufreq_umbrella_core_speedchange_task, NULL,
			       "cfumbrella_core");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process(speedchange_task);
#ifdef CONFIG_HAS_EARLYSUSPEND
    register_early_suspend(&cpufreq_umbrella_core_power_suspend_info);
#endif
	return cpufreq_register_governor(CPU_FREQ_GOV_UMBRELLA_CORE);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_UMBRELLA_CORE
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return CPU_FREQ_GOV_UMBRELLA_CORE;
}

fs_initcall(cpufreq_umbrella_core_gov_init);
#else
module_init(cpufreq_umbrella_core_gov_init);
#endif

#ifdef CONFIG_UC_MODE_AUTO_CHANGE_BOOST
static void mode_auto_change_boost(struct work_struct *work)
{
	if(mode_count == 1) {
		request_bimc_clk(bimc_hispeed_freq);
		msm_pm_retention_mode_enable(0);
	}
	else if(mode_count == 0) {
		request_bimc_clk(0);
		msm_pm_retention_mode_enable(1);
	}
}
#endif	// CONFIG_UC_MODE_AUTO_CHANGE_BOOST

static void __exit cpufreq_umbrella_core_gov_exit(void)
{
	cpufreq_unregister_governor(CPU_FREQ_GOV_UMBRELLA_CORE);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);
}

module_exit(cpufreq_umbrella_core_gov_exit);

MODULE_AUTHOR("AbandonedCart <twistedumbrella@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_umbrella_core' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPL");
