/* FeraLab */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/moduleparam.h>
#include <asm/cputime.h>
#include <linux/earlysuspend.h>

#define DEFAULT_AWAKE_IDEAL_FREQ 998400
static unsigned int awake_ideal_freq;
#define DEFAULT_SLEEP_IDEAL_FREQ 245760
static unsigned int sleep_ideal_freq;
#define DEFAULT_RAMP_UP_STEP 256000
static unsigned int ramp_up_step;
#define DEFAULT_RAMP_DOWN_STEP 256000
static unsigned int ramp_down_step;
#define DEFAULT_MAX_CPU_LOAD 54
static unsigned long max_cpu_load;
#define DEFAULT_MIN_CPU_LOAD 15
static unsigned long min_cpu_load;
#define DEFAULT_UP_RATE_US 45000;
static unsigned long up_rate_us;
#define DEFAULT_DOWN_RATE_US 45000;
static unsigned long down_rate_us;
#define DEFAULT_SLEEP_WAKEUP_FREQ 768000
static unsigned int sleep_wakeup_freq;
#define DEFAULT_SAMPLE_RATE_JIFFIES 2
static unsigned int sample_rate_jiffies;

static void (*pm_idle_old)(void);
static atomic_t active_count = ATOMIC_INIT(0);

struct smartass_info_s {
	struct cpufreq_policy *cur_policy;
	struct cpufreq_frequency_table *freq_table;
	struct timer_list timer;
	u64 time_in_idle;
	u64 idle_exit_time;
	u64 freq_change_time;
	u64 freq_change_time_in_idle;
	int cur_cpu_load;
	int old_freq;
	int ramp_dir;
	unsigned int enable;
	int ideal_speed;
};

static DEFINE_PER_CPU(struct smartass_info_s, smartass_info);
static struct workqueue_struct *up_wq;
static struct workqueue_struct *down_wq;
static struct work_struct freq_scale_work;
static cpumask_t work_cpumask;
static spinlock_t cpumask_lock;
static unsigned int suspended;

enum {
	SMARTASS_DEBUG_JUMPS=1,
	SMARTASS_DEBUG_LOAD=2,
	SMARTASS_DEBUG_ALG=4
};

static unsigned long debug_mask;
static int cpufreq_governor_smartass_h3(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SMARTASSH3
static
#endif

struct cpufreq_governor cpufreq_gov_smartass_h3 = {
	.name = "smartassH3",
	.governor = cpufreq_governor_smartass_h3,
	.max_transition_latency = 9000000,
	.owner = THIS_MODULE,
};

inline static void smartass_update_min_max(struct smartass_info_s *this_smartass, struct cpufreq_policy *policy, int suspend) {
	if (suspend) {
		this_smartass->ideal_speed =
			policy->max > sleep_ideal_freq ?
			(sleep_ideal_freq > policy->min ? sleep_ideal_freq : policy->min) : policy->max;
	} else {
		this_smartass->ideal_speed =
			policy->min < awake_ideal_freq ?
			(awake_ideal_freq < policy->max ? awake_ideal_freq : policy->max) : policy->min;
	}
}

inline static void smartass_update_min_max_allcpus(void) {
	unsigned int i;
	for_each_online_cpu(i) {
		struct smartass_info_s *this_smartass = &per_cpu(smartass_info, i);
		if (this_smartass->enable)
			smartass_update_min_max(this_smartass,this_smartass->cur_policy,suspended);
	}
}

inline static unsigned int validate_freq(struct cpufreq_policy *policy, int freq) {
	if (freq > (int)policy->max)
		return policy->max;
	if (freq < (int)policy->min)
		return policy->min;
	return freq;
}

inline static void reset_timer(unsigned long cpu, struct smartass_info_s *this_smartass) {
	this_smartass->time_in_idle = get_cpu_idle_time_us(cpu, &this_smartass->idle_exit_time);
	mod_timer(&this_smartass->timer, jiffies + sample_rate_jiffies);
}

inline static void work_cpumask_set(unsigned long cpu) {
	unsigned long flags;
	spin_lock_irqsave(&cpumask_lock, flags);
	cpumask_set_cpu(cpu, &work_cpumask);
	spin_unlock_irqrestore(&cpumask_lock, flags);
}

inline static int work_cpumask_test_and_clear(unsigned long cpu) {
	unsigned long flags;
	int res = 0;
	spin_lock_irqsave(&cpumask_lock, flags);
	res = test_and_clear_bit(cpu, work_cpumask.bits);
	spin_unlock_irqrestore(&cpumask_lock, flags);
	return res;
}

inline static int target_freq(struct cpufreq_policy *policy, struct smartass_info_s *this_smartass,
			      int new_freq, int old_freq, int prefered_relation) {
	int index, target;
	struct cpufreq_frequency_table *table = this_smartass->freq_table;

	if (new_freq == old_freq)
		return 0;
	new_freq = validate_freq(policy,new_freq);
	if (new_freq == old_freq)
		return 0;

	if (table &&
	    !cpufreq_frequency_table_target(policy,table,new_freq,prefered_relation,&index))
	{
		target = table[index].frequency;
		if (target == old_freq) {
			if (new_freq > old_freq && prefered_relation==CPUFREQ_RELATION_H
			    && !cpufreq_frequency_table_target(policy,table,new_freq,
							       CPUFREQ_RELATION_L,&index))
				target = table[index].frequency;
			else if (new_freq < old_freq && prefered_relation==CPUFREQ_RELATION_L
				&& !cpufreq_frequency_table_target(policy,table,new_freq,
								   CPUFREQ_RELATION_H,&index))
				target = table[index].frequency;
		}

		if (target == old_freq) {
			return 0;
		}
	}
	else target = new_freq;
	__cpufreq_driver_target(policy, target, prefered_relation);
	return target;
}

static void cpufreq_smartass_timer(unsigned long cpu)
{
	u64 delta_idle;
	u64 delta_time;
	int cpu_load;
	int old_freq;
	u64 update_time;
	u64 now_idle;
	int queued_work = 0;
	struct smartass_info_s *this_smartass = &per_cpu(smartass_info, cpu);
	struct cpufreq_policy *policy = this_smartass->cur_policy;

	now_idle = get_cpu_idle_time_us(cpu, &update_time);
	old_freq = policy->cur;

	if (this_smartass->idle_exit_time == 0 || update_time == this_smartass->idle_exit_time)
		return;

	delta_idle = cputime64_sub(now_idle, this_smartass->time_in_idle);
	delta_time = cputime64_sub(update_time, this_smartass->idle_exit_time);

	if (delta_time < 1000) {
		if (!timer_pending(&this_smartass->timer))
			reset_timer(cpu,this_smartass);
		return;
	}

	if (delta_idle > delta_time)
		cpu_load = 0;
	else
		cpu_load = 100 * (unsigned int)(delta_time - delta_idle) / (unsigned int)delta_time;
	this_smartass->cur_cpu_load = cpu_load;
	this_smartass->old_freq = old_freq;

	if (cpu_load > max_cpu_load || delta_idle == 0)
	{
		if (old_freq < policy->max &&
			 (old_freq < this_smartass->ideal_speed || delta_idle == 0 ||
			  cputime64_sub(update_time, this_smartass->freq_change_time) >= up_rate_us))
		{
			this_smartass->ramp_dir = 1;
			work_cpumask_set(cpu);
			queue_work(up_wq, &freq_scale_work);
			queued_work = 1;
		}
		else this_smartass->ramp_dir = 0;
	}
	else if (cpu_load < min_cpu_load && old_freq > policy->min &&
		 (old_freq > this_smartass->ideal_speed ||
		  cputime64_sub(update_time, this_smartass->freq_change_time) >= down_rate_us))
	{
		this_smartass->ramp_dir = -1;
		work_cpumask_set(cpu);
		queue_work(down_wq, &freq_scale_work);
		queued_work = 1;
	}
	else this_smartass->ramp_dir = 0;

	if (!queued_work && old_freq < policy->max)
		reset_timer(cpu,this_smartass);
}

static void cpufreq_idle(void)
{
	struct smartass_info_s *this_smartass = &per_cpu(smartass_info, smp_processor_id());
	struct cpufreq_policy *policy = this_smartass->cur_policy;

	if (!this_smartass->enable) {
		pm_idle_old();
		return;
	}

	if (policy->cur == policy->min && timer_pending(&this_smartass->timer))
		del_timer(&this_smartass->timer);

	pm_idle_old();

	if (!timer_pending(&this_smartass->timer))
		reset_timer(smp_processor_id(), this_smartass);
}

static void cpufreq_smartass_freq_change_time_work(struct work_struct *work)
{
	unsigned int cpu;
	int new_freq;
	int old_freq;
	int ramp_dir;
	struct smartass_info_s *this_smartass;
	struct cpufreq_policy *policy;
	unsigned int relation = CPUFREQ_RELATION_L;
	for_each_possible_cpu(cpu) {
		this_smartass = &per_cpu(smartass_info, cpu);
		if (!work_cpumask_test_and_clear(cpu))
			continue;

		ramp_dir = this_smartass->ramp_dir;
		this_smartass->ramp_dir = 0;

		old_freq = this_smartass->old_freq;
		policy = this_smartass->cur_policy;

		if (old_freq != policy->cur) {
			new_freq = old_freq;
		}
		else if (ramp_dir > 0 && nr_running() > 1) {
			if (old_freq < this_smartass->ideal_speed)
				new_freq = this_smartass->ideal_speed;
			else if (ramp_up_step) {
				new_freq = old_freq + ramp_up_step;
				relation = CPUFREQ_RELATION_H;
			}
			else {
				new_freq = policy->max;
				relation = CPUFREQ_RELATION_H;
			}
		}
		else if (ramp_dir < 0) {
			if (old_freq > this_smartass->ideal_speed) {
				new_freq = this_smartass->ideal_speed;
				relation = CPUFREQ_RELATION_H;
			}
			else if (ramp_down_step)
				new_freq = old_freq - ramp_down_step;
			else {
				new_freq = old_freq * this_smartass->cur_cpu_load / max_cpu_load;
				if (new_freq > old_freq)
					new_freq = old_freq -1;
			}
		}
		else {
			new_freq = old_freq;
		}

		new_freq = target_freq(policy,this_smartass,new_freq,old_freq,relation);
		if (new_freq)
			this_smartass->freq_change_time_in_idle =
				get_cpu_idle_time_us(cpu,&this_smartass->freq_change_time);

		if (new_freq < policy->max)
			reset_timer(cpu,this_smartass);
		else if (timer_pending(&this_smartass->timer))
			del_timer(&this_smartass->timer);
	}
}

static ssize_t show_debug_mask(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%lu\n", debug_mask);
}

static ssize_t store_debug_mask(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0)
		debug_mask = input;
	return count;
}

static ssize_t show_up_rate_us(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%lu\n", up_rate_us);
}

static ssize_t store_up_rate_us(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0 && input <= 100000000)
		up_rate_us = input;
	return count;
}

static ssize_t show_down_rate_us(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%lu\n", down_rate_us);
}

static ssize_t store_down_rate_us(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0 && input <= 100000000)
		down_rate_us = input;
	return count;
}

static ssize_t show_sleep_ideal_freq(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", sleep_ideal_freq);
}

static ssize_t store_sleep_ideal_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0) {
		sleep_ideal_freq = input;
		if (suspended)
			smartass_update_min_max_allcpus();
	}
	return count;
}

static ssize_t show_sleep_wakeup_freq(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", sleep_wakeup_freq);
}

static ssize_t store_sleep_wakeup_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0)
		sleep_wakeup_freq = input;
	return count;
}

static ssize_t show_awake_ideal_freq(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", awake_ideal_freq);
}

static ssize_t store_awake_ideal_freq(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0) {
		awake_ideal_freq = input;
		if (!suspended)
			smartass_update_min_max_allcpus();
	}
	return count;
}

static ssize_t show_sample_rate_jiffies(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", sample_rate_jiffies);
}

static ssize_t store_sample_rate_jiffies(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input > 0 && input <= 1000)
		sample_rate_jiffies = input;
	return count;
}

static ssize_t show_ramp_up_step(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", ramp_up_step);
}

static ssize_t store_ramp_up_step(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0)
		ramp_up_step = input;
	return count;
}

static ssize_t show_ramp_down_step(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%u\n", ramp_down_step);
}

static ssize_t store_ramp_down_step(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input >= 0)
		ramp_down_step = input;
	return count;
}

static ssize_t show_max_cpu_load(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%lu\n", max_cpu_load);
}

static ssize_t store_max_cpu_load(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input > 0 && input <= 100)
		max_cpu_load = input;
	return count;
}

static ssize_t show_min_cpu_load(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%lu\n", min_cpu_load);
}

static ssize_t store_min_cpu_load(struct cpufreq_policy *policy, const char *buf, size_t count)
{
	ssize_t res;
	unsigned long input;
	res = strict_strtoul(buf, 0, &input);
	if (res >= 0 && input > 0 && input < 100)
		min_cpu_load = input;
	return count;
}

#define define_global_rw_attr(_name)		\
static struct freq_attr _name##_attr =		\
	__ATTR(_name, 0644, show_##_name, store_##_name)

define_global_rw_attr(debug_mask);
define_global_rw_attr(up_rate_us);
define_global_rw_attr(down_rate_us);
define_global_rw_attr(sleep_ideal_freq);
define_global_rw_attr(sleep_wakeup_freq);
define_global_rw_attr(awake_ideal_freq);
define_global_rw_attr(sample_rate_jiffies);
define_global_rw_attr(ramp_up_step);
define_global_rw_attr(ramp_down_step);
define_global_rw_attr(max_cpu_load);
define_global_rw_attr(min_cpu_load);

static struct attribute * smartass_attributes[] = {
	&debug_mask_attr.attr,
	&up_rate_us_attr.attr,
	&down_rate_us_attr.attr,
	&sleep_ideal_freq_attr.attr,
	&sleep_wakeup_freq_attr.attr,
	&awake_ideal_freq_attr.attr,
	&sample_rate_jiffies_attr.attr,
	&ramp_up_step_attr.attr,
	&ramp_down_step_attr.attr,
	&max_cpu_load_attr.attr,
	&min_cpu_load_attr.attr,
	NULL,
};

static struct attribute_group smartass_attr_group = {
	.attrs = smartass_attributes,
	.name = "smartassH3",
};

static int cpufreq_governor_smartass_h3(struct cpufreq_policy *new_policy, unsigned int event)
{
	unsigned int cpu = new_policy->cpu;
	int rc;
	struct smartass_info_s *this_smartass = &per_cpu(smartass_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!new_policy->cur))
			return -EINVAL;

		this_smartass->cur_policy = new_policy;

		this_smartass->enable = 1;

		smartass_update_min_max(this_smartass,new_policy,suspended);

		this_smartass->freq_table = cpufreq_frequency_get_table(cpu);
		if (!this_smartass->freq_table)
		smp_wmb();

		if (atomic_inc_return(&active_count) <= 1) {
			rc = sysfs_create_group(&new_policy->kobj, &smartass_attr_group);
			if (rc)
				return rc;

			pm_idle_old = pm_idle;
			pm_idle = cpufreq_idle;
		}

		if (this_smartass->cur_policy->cur < new_policy->max && !timer_pending(&this_smartass->timer))
			reset_timer(cpu,this_smartass);

		break;

	case CPUFREQ_GOV_LIMITS:
		smartass_update_min_max(this_smartass,new_policy,suspended);

		if (this_smartass->cur_policy->cur > new_policy->max) {
			__cpufreq_driver_target(this_smartass->cur_policy,
						new_policy->max, CPUFREQ_RELATION_H);
		}
		else if (this_smartass->cur_policy->cur < new_policy->min) {
			__cpufreq_driver_target(this_smartass->cur_policy,
						new_policy->min, CPUFREQ_RELATION_L);
		}

		if (this_smartass->cur_policy->cur < new_policy->max && !timer_pending(&this_smartass->timer))
			reset_timer(cpu,this_smartass);

		break;

	case CPUFREQ_GOV_STOP:
		this_smartass->enable = 0;
		smp_wmb();
		del_timer(&this_smartass->timer);
		flush_work(&freq_scale_work);
		this_smartass->idle_exit_time = 0;

		if (atomic_dec_return(&active_count) <= 1) {
			sysfs_remove_group(&new_policy->kobj,
					   &smartass_attr_group);
			pm_idle = pm_idle_old;
		}
		break;
	}

	return 0;
}

static void smartass_suspend(int cpu, int suspend)
{
	struct smartass_info_s *this_smartass = &per_cpu(smartass_info, smp_processor_id());
	struct cpufreq_policy *policy = this_smartass->cur_policy;
	unsigned int new_freq;

	if (!this_smartass->enable)
		return;

	smartass_update_min_max(this_smartass,policy,suspend);
	if (!suspend) {
		new_freq = validate_freq(policy,sleep_wakeup_freq);
		__cpufreq_driver_target(policy, new_freq,
					CPUFREQ_RELATION_L);
	} else {
		this_smartass->freq_change_time_in_idle =
			get_cpu_idle_time_us(cpu,&this_smartass->freq_change_time);
	}

	reset_timer(smp_processor_id(),this_smartass);
}

static void smartass_early_suspend(struct early_suspend *handler) {
	int i;
	if (suspended || sleep_ideal_freq==0)
		return;
	suspended = 1;
	for_each_online_cpu(i)
		smartass_suspend(i,1);
}

static void smartass_late_resume(struct early_suspend *handler) {
	int i;
	if (!suspended)
		return;
	suspended = 0;
	for_each_online_cpu(i)
		smartass_suspend(i,0);
}

static struct early_suspend smartass_power_suspend = {
	.suspend = smartass_early_suspend,
	.resume = smartass_late_resume,
};

static int __init cpufreq_smartass_init(void)
{
	unsigned int i;
	struct smartass_info_s *this_smartass;
	debug_mask = 0;
	up_rate_us = DEFAULT_UP_RATE_US;
	down_rate_us = DEFAULT_DOWN_RATE_US;
	sleep_ideal_freq = DEFAULT_SLEEP_IDEAL_FREQ;
	sleep_wakeup_freq = DEFAULT_SLEEP_WAKEUP_FREQ;
	awake_ideal_freq = DEFAULT_AWAKE_IDEAL_FREQ;
	sample_rate_jiffies = DEFAULT_SAMPLE_RATE_JIFFIES;
	ramp_up_step = DEFAULT_RAMP_UP_STEP;
	ramp_down_step = DEFAULT_RAMP_DOWN_STEP;
	max_cpu_load = DEFAULT_MAX_CPU_LOAD;
	min_cpu_load = DEFAULT_MIN_CPU_LOAD;
	spin_lock_init(&cpumask_lock);
	suspended = 0;

	for_each_possible_cpu(i) {
		this_smartass = &per_cpu(smartass_info, i);
		this_smartass->enable = 0;
		this_smartass->cur_policy = 0;
		this_smartass->ramp_dir = 0;
		this_smartass->time_in_idle = 0;
		this_smartass->idle_exit_time = 0;
		this_smartass->freq_change_time = 0;
		this_smartass->freq_change_time_in_idle = 0;
		this_smartass->cur_cpu_load = 0;
		init_timer_deferrable(&this_smartass->timer);
		this_smartass->timer.function = cpufreq_smartass_timer;
		this_smartass->timer.data = i;
		work_cpumask_test_and_clear(i);
	}

	up_wq = create_rt_workqueue("ksmartass_up");
	down_wq = create_workqueue("ksmartass_down");
	if (!up_wq || !down_wq)
		return -ENOMEM;

	INIT_WORK(&freq_scale_work, cpufreq_smartass_freq_change_time_work);

	register_early_suspend(&smartass_power_suspend);

	return cpufreq_register_governor(&cpufreq_gov_smartass_h3);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SMARTASSH3
fs_initcall(cpufreq_smartass_init);
#else
module_init(cpufreq_smartass_init);
#endif

static void __exit cpufreq_smartass_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_smartass_h3);
	destroy_workqueue(up_wq);
	destroy_workqueue(down_wq);
}

module_exit(cpufreq_smartass_exit);
MODULE_AUTHOR ("Erasmux, moded by FeraVolt");
MODULE_DESCRIPTION ("'cpufreq_smartassH3' - A smart cpufreq governor");
MODULE_LICENSE ("GPL");

