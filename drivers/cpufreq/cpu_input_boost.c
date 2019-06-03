// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
// Copyright (C) 2019 GPU input boost, stune boost, Erik Müller <pappschlumpf@xda>

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_drm_notify.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include "../gpu/msm/kgsl.h"
#include "../gpu/msm/kgsl_pwrscale.h"
#include "../gpu/msm/kgsl_device.h"
#include <linux/version.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned int input_boost_freq_lp __read_mostly = CONFIG_INPUT_BOOST_FREQ_LP;
static unsigned int input_boost_freq_hp __read_mostly = CONFIG_INPUT_BOOST_FREQ_PERF;
static unsigned int input_boost_freq_gold __read_mostly = CONFIG_INPUT_BOOST_FREQ_GOLD;
static unsigned int flex_boost_freq_lp __read_mostly = CONFIG_FLEX_BOOST_FREQ_LP;
static unsigned int flex_boost_freq_hp __read_mostly = CONFIG_FLEX_BOOST_FREQ_PERF;
static unsigned int flex_boost_freq_gold __read_mostly = CONFIG_FLEX_BOOST_FREQ_GOLD;
static unsigned int max_boost_freq_lp __read_mostly = CONFIG_MAX_BOOST_FREQ_LP;
static unsigned int max_boost_freq_hp __read_mostly = CONFIG_MAX_BOOST_FREQ_PERF;
static unsigned int max_boost_freq_gold __read_mostly = CONFIG_MAX_BOOST_FREQ_GOLD;
static unsigned int remove_input_boost_freq_lp __read_mostly = CONFIG_REMOVE_INPUT_BOOST_FREQ_LP;
static unsigned int remove_input_boost_freq_perf __read_mostly = CONFIG_REMOVE_INPUT_BOOST_FREQ_PERF;
static unsigned int remove_input_boost_freq_gold __read_mostly = CONFIG_REMOVE_INPUT_BOOST_FREQ_GOLD;
static unsigned int gpu_boost_freq __read_mostly = CONFIG_GPU_BOOST_FREQ;
static unsigned int gpu_min_freq __read_mostly = CONFIG_GPU_MIN_FREQ;
static unsigned short input_boost_duration __read_mostly = CONFIG_INPUT_BOOST_DURATION_MS;
static unsigned short flex_boost_duration __read_mostly = CONFIG_FLEX_BOOST_DURATION_MS;
static  unsigned int input_thread_prio __read_mostly = CONFIG_INPUT_THREAD_PRIORITY;
static unsigned int gpu_boost_extender_ms __read_mostly = CONFIG_GPU_BOOST_EXTENDER_MS;
static bool little_only __read_mostly = false;
static bool boost_gold __read_mostly = true;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static short dynamic_stune_boost __read_mostly = 20;
static short input_stune_boost_offset __read_mostly = CONFIG_INPUT_BOOST_STUNE_OFFSET;
static short max_stune_boost_offset __read_mostly = CONFIG_MAX_BOOST_STUNE_OFFSET;
static short flex_stune_boost_offset __read_mostly = CONFIG_FLEX_BOOST_STUNE_OFFSET;
static unsigned int stune_boost_extender_ms __read_mostly = CONFIG_STUNE_BOOST_EXTENDER_MS;
static unsigned int max_stune_boost_extender_ms __read_mostly = CONFIG_MAX_STUNE_BOOST_EXTENDER_MS;

module_param(dynamic_stune_boost, short, 0644);
module_param(input_stune_boost_offset, short, 0644);
module_param(max_stune_boost_offset, short, 0644);
module_param(flex_stune_boost_offset, short, 0644);
module_param(gpu_boost_freq, uint, 0644);
module_param(gpu_min_freq, uint, 0644);
module_param(max_stune_boost_extender_ms, uint, 0644);
module_param(stune_boost_extender_ms, uint, 0644);
#endif

module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param(input_boost_freq_gold, uint, 0644);
module_param(flex_boost_freq_lp, uint, 0644);
module_param(flex_boost_freq_hp, uint, 0644);
module_param(flex_boost_freq_gold, uint, 0644);
module_param(remove_input_boost_freq_lp, uint, 0644);
module_param(remove_input_boost_freq_perf, uint, 0644);
module_param(remove_input_boost_freq_gold, uint, 0644);
module_param(max_boost_freq_lp, uint, 0644);
module_param(max_boost_freq_hp, uint, 0644);
module_param(max_boost_freq_gold, uint, 0644);
module_param(input_boost_duration, short, 0644);
module_param(flex_boost_duration, short, 0644);
module_param(gpu_boost_extender_ms, uint, 0644);
module_param(little_only, bool, 0644);
module_param(boost_gold, bool, 0644);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	FLEX_BOOST,
	CLUSTER1_BOOST,
	CLUSTER2_BOOST,
	CLUSTER1_WAKE_BOOST,
	CLUSTER2_WAKE_BOOST,
	INPUT_STUNE_BOOST,
	MAX_STUNE_BOOST,
	FLEX_STUNE_BOOST
};

struct boost_drv {
	struct workqueue_struct *wq_i;
	struct workqueue_struct *wq_f;
	struct workqueue_struct *wq_cl1;
	struct workqueue_struct *wq_cl2;
	struct workqueue_struct *wq_istu;
	struct workqueue_struct *wq_mstu;
	struct workqueue_struct *wq_gpu;
	struct delayed_work input_unboost;
	struct delayed_work flex_unboost;
	struct delayed_work cluster1_unboost;
	struct delayed_work cluster2_unboost;
	struct delayed_work input_stune_unboost;
	struct delayed_work max_stune_unboost;
	struct delayed_work gpu_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	struct kgsl_device *gpu_device;
	struct kgsl_pwrctrl *gpu_pwr;
	int input_stune_slot;
	int max_stune_slot;
	int flex_stune_slot;
	wait_queue_head_t boost_waitq;
	unsigned long state;
	unsigned long stune_state;
	//atomic64_t cluster1_boost_expires;
	//atomic64_t cluster2_boost_expires;
};

static void input_unboost_worker(struct work_struct *work);
static void flex_unboost_worker(struct work_struct *work);
static void cluster1_unboost_worker(struct work_struct *work);
static void cluster2_unboost_worker(struct work_struct *work);
static void input_stune_unboost_worker(struct work_struct *work);
static void max_stune_unboost_worker(struct work_struct *work);
static void gpu_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.flex_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.flex_unboost,
						  flex_unboost_worker, 0),
	.cluster1_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.cluster1_unboost,
						  cluster1_unboost_worker, 0),
	.cluster2_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.cluster2_unboost,
						  cluster2_unboost_worker, 0),
	.input_stune_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_stune_unboost,
						  input_stune_unboost_worker, 0),
	.max_stune_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_stune_unboost,
						  max_stune_unboost_worker, 0),
	.gpu_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.gpu_unboost,
						  gpu_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

static unsigned int get_input_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = input_boost_freq_lp;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = input_boost_freq_hp;
	else freq =  input_boost_freq_gold; 

	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = max_boost_freq_lp;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = max_boost_freq_hp;
	else freq = max_boost_freq_gold;

	return min(freq, policy->max);
}

static unsigned int get_flex_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = flex_boost_freq_lp;
	else if (cpumask_test_cpu(policy->cpu, cpu_perf_mask))
		freq = flex_boost_freq_hp;
	else freq =  flex_boost_freq_gold; 

	return min(freq, policy->max);
}

static unsigned int get_min_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return remove_input_boost_freq_lp;

	if (cpumask_test_cpu(cpu, cpu_perf_mask))
		return remove_input_boost_freq_perf;

	return remove_input_boost_freq_gold;
}

static void update_online_cpu_policy(void)
{
	u32 cpu;
	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_gold_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void update_stune_boost(struct boost_drv *b, int BIT, int level,
			    int *slot)
{
	if (level && !test_bit(BIT, &b->stune_state)) {
		if (!do_stune_boost("top-app", level, slot))
			set_bit(BIT, &b->stune_state);
	}
}

static void clear_stune_boost(struct boost_drv *b, u32 BIT, int slot)
{
	if (test_bit(BIT, &b->stune_state)) {
		reset_stune_boost("top-app", slot);
		clear_bit(BIT, &b->stune_state);
	}
}

static void update_gpu_boost(struct boost_drv *b, int freq)
{
	int level;
	if (gpu_boost_freq==0) return;
	if (freq==427)
		level=5;
	if (freq==345)
		level=6;
	if (freq==257)
		level=7;
	mutex_lock(&b->gpu_device->mutex);
	b->gpu_pwr->min_pwrlevel=level;
	mutex_unlock(&b->gpu_device->mutex);
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	if (!mod_delayed_work(b->wq_i, &b->input_unboost,
			msecs_to_jiffies(input_boost_duration))) {
		set_bit(INPUT_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (!mod_delayed_work(b->wq_gpu, &b->gpu_unboost,
			msecs_to_jiffies(input_boost_duration+gpu_boost_extender_ms)))
		update_gpu_boost(b, gpu_boost_freq);
	if (!mod_delayed_work(b->wq_istu, &b->input_stune_unboost,
			msecs_to_jiffies(input_boost_duration+stune_boost_extender_ms)))
		update_stune_boost(b, INPUT_STUNE_BOOST, dynamic_stune_boost+input_stune_boost_offset,
				&b->input_stune_slot);	
}

static void __cpu_input_boost_kick_cluster1(struct boost_drv *b,
				       unsigned int duration_ms)
{
	if (!mod_delayed_work(b->wq_cl1, &b->cluster1_unboost,
			      msecs_to_jiffies(duration_ms))) {
		set_bit(CLUSTER1_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (!mod_delayed_work(b->wq_mstu, &b->max_stune_unboost,
			msecs_to_jiffies(duration_ms+max_stune_boost_extender_ms)))
		update_stune_boost(b, MAX_STUNE_BOOST, dynamic_stune_boost+max_stune_boost_offset,
				&b->max_stune_slot);
}

static void __cpu_input_boost_kick_cluster2(struct boost_drv *b,
				       unsigned int duration_ms)
{
	if (!mod_delayed_work(b->wq_cl2, &b->cluster2_unboost,
			msecs_to_jiffies(duration_ms))) {
		set_bit(CLUSTER2_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (!test_bit(CLUSTER1_BOOST, &b->state) || !test_bit(CLUSTER1_WAKE_BOOST, &b->state)) {
		if (!mod_delayed_work(b->wq_mstu, &b->max_stune_unboost,
				msecs_to_jiffies(duration_ms+max_stune_boost_extender_ms)))
			update_stune_boost(b, MAX_STUNE_BOOST, dynamic_stune_boost+max_stune_boost_offset,
					&b->max_stune_slot);
	}
}

void cpu_input_boost_kick_cluster1(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	if (duration_ms == 0)
		return;

	if (test_bit(SCREEN_OFF, &b->state))
		return;
	
	__cpu_input_boost_kick_cluster1(b, duration_ms);
}

void cpu_input_boost_kick_cluster2(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	if (little_only || duration_ms == 0)
		return;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	__cpu_input_boost_kick_cluster2(b, duration_ms);
}

static void __cpu_input_boost_kick_cluster1_wake(struct boost_drv *b,
				       unsigned int duration_ms)
{
	if (!mod_delayed_work(b->wq_cl1, &b->cluster1_unboost,
			msecs_to_jiffies(duration_ms))) {
		set_bit(CLUSTER1_WAKE_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (!mod_delayed_work(b->wq_mstu, &b->max_stune_unboost,
			msecs_to_jiffies(duration_ms+max_stune_boost_extender_ms)))
		update_stune_boost(b, MAX_STUNE_BOOST, dynamic_stune_boost+max_stune_boost_offset,
				&b->max_stune_slot);	
}

static void __cpu_input_boost_kick_cluster2_wake(struct boost_drv *b,
				       unsigned int duration_ms)
{
	if (!mod_delayed_work(b->wq_cl2, &b->cluster2_unboost,
			msecs_to_jiffies(duration_ms))) {
		set_bit(CLUSTER2_WAKE_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (!test_bit(CLUSTER1_WAKE_BOOST, &b->state) || !test_bit(CLUSTER1_BOOST, &b->state)) {
		if (!mod_delayed_work(b->wq_mstu, &b->max_stune_unboost,
				msecs_to_jiffies(duration_ms+max_stune_boost_extender_ms)))
			update_stune_boost(b, MAX_STUNE_BOOST, dynamic_stune_boost+max_stune_boost_offset,
					&b->max_stune_slot);
	}
}

void cpu_input_boost_kick_cluster1_wake(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	if (duration_ms == 0)
		return;

	if (!test_bit(SCREEN_OFF, &b->state))
		return;

	__cpu_input_boost_kick_cluster1_wake(b, duration_ms);
}

void cpu_input_boost_kick_cluster2_wake(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	if (little_only || duration_ms == 0)
		return;

	if (!test_bit(SCREEN_OFF, &b->state))
		return;

	__cpu_input_boost_kick_cluster2_wake(b, duration_ms);
}

static void __cpu_input_boost_kick_flex(struct boost_drv *b)
{
	if (!mod_delayed_work(b->wq_f, &b->flex_unboost,
			msecs_to_jiffies(flex_boost_duration))) {
		set_bit(FLEX_BOOST, &b->state);
		wake_up(&b->boost_waitq);
	}
	if (dynamic_stune_boost+flex_stune_boost_offset > 0)
			update_stune_boost(b, FLEX_STUNE_BOOST, dynamic_stune_boost+flex_stune_boost_offset,
				&b->flex_stune_slot);
}

void cpu_input_boost_kick_flex(void)
{
	struct boost_drv *b = &boost_drv_g;
	
	if (test_bit(SCREEN_OFF, &b->state))
		return;

	__cpu_input_boost_kick_flex(b);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);
	
	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void cluster1_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), cluster1_unboost);

	clear_bit(CLUSTER1_WAKE_BOOST, &b->state);
	clear_bit(CLUSTER1_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void cluster2_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), cluster2_unboost);
	
	clear_bit(CLUSTER2_WAKE_BOOST, &b->state);
	clear_bit(CLUSTER2_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void flex_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), flex_unboost);

	clear_bit(FLEX_BOOST, &b->state);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	clear_stune_boost(b, FLEX_STUNE_BOOST, b->flex_stune_slot);
#endif
	wake_up(&b->boost_waitq);
}

static void input_stune_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_stune_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	clear_stune_boost(b, INPUT_STUNE_BOOST, b->input_stune_slot);
#endif
}

static void max_stune_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_stune_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	clear_stune_boost(b, MAX_STUNE_BOOST, b->max_stune_slot);
#endif
}

static void gpu_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), gpu_unboost);

	update_gpu_boost(b, gpu_min_freq);
}

static int cpu_boost_thread(void *data)
{
	static struct sched_param sched_max_rt_prio;
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	if (input_thread_prio == 99)
 		sched_max_rt_prio.sched_priority = MAX_RT_PRIO - 1;
	else 
		sched_max_rt_prio.sched_priority = input_thread_prio;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (!kthread_should_stop()) {
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			kthread_should_stop());
		old_state = curr_state;
		update_online_cpu_policy();
	}
	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	unsigned int min_freq;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	if (test_bit(CLUSTER1_WAKE_BOOST, &b->state) && (policy->cpu < 4)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}
	if (test_bit(CLUSTER2_WAKE_BOOST, &b->state)) {
		if ((policy->cpu > 3) && (policy->cpu < 7))
			policy->min = get_max_boost_freq(policy);
		if ((policy->cpu  == 7) && boost_gold) 
			policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	/* Unboost when the screen is off */
	if (test_bit(SCREEN_OFF, &b->state)) {
		min_freq = get_min_freq(b, policy->cpu);
		policy->min = max(policy->cpuinfo.min_freq, min_freq);
		return NOTIFY_OK;
	}

	/* Boost CPU to max frequency for max boost */
	if (test_bit(CLUSTER1_BOOST, &b->state) && (policy->cpu < 4)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}
	if (test_bit(CLUSTER2_BOOST, &b->state)) {
		if ((policy->cpu > 3) && (policy->cpu < 7))
			policy->min = get_max_boost_freq(policy);
		if ((policy->cpu  == 7) && boost_gold)
			policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	if (test_bit(INPUT_BOOST, &b->state) || test_bit(FLEX_BOOST, &b->state)) {
		if (test_bit(FLEX_BOOST, &b->state)) {
			if (policy->cpu < 4)
				policy->min = get_flex_boost_freq(policy);
			if (policy->cpu > 3)
				policy->min = get_flex_boost_freq(policy);
		}

		if (test_bit(INPUT_BOOST, &b->state)) {
			if (policy->cpu < 4)
				policy->min = get_input_boost_freq(policy);
			if (policy->cpu > 3)
				policy->min = get_input_boost_freq(policy);
		}
		return NOTIFY_OK;
	}

	min_freq = get_min_freq(b, policy->cpu);
	policy->min = max(policy->cpuinfo.min_freq, min_freq);

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	clear_bit(SCREEN_OFF, &b->state);

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK_CUST) {	
		cpu_input_boost_kick_cluster1_wake(500);
		cpu_input_boost_kick_cluster2_wake(500);	
		clear_bit(SCREEN_OFF, &b->state);
	} else {
		set_bit(SCREEN_OFF, &b->state);
	}
	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct task_struct *boost_thread;
	struct boost_drv *b = &boost_drv_g;
	int ret;
	
	b->wq_i = alloc_workqueue("cpu_input_boost_wq_i", WQ_HIGHPRI, 0);
	if (!b->wq_i) {
		ret = -ENOMEM;
		return ret;
	}
	
	b->wq_f = alloc_workqueue("cpu_input_boost_wq_f", WQ_HIGHPRI, 0);
	if (!b->wq_f) {
		ret = -ENOMEM;
		return ret;
	}
	
	b->wq_cl1 = alloc_workqueue("cpu_input_boost_wq_cl1", WQ_HIGHPRI, 0);
	if (!b->wq_cl1) {
		ret = -ENOMEM;
		return ret;
	}
	
	b->wq_cl2 = alloc_workqueue("cpu_input_boost_wq_cl2", WQ_HIGHPRI, 0);
	if (!b->wq_cl2) {
		ret = -ENOMEM;
		return ret;
	}

	b->wq_istu = alloc_workqueue("cpu_input_boost_wq_istu", WQ_HIGHPRI, 0);
	if (!b->wq_istu) {
		ret = -ENOMEM;
		return ret;
	}

	b->wq_mstu = alloc_workqueue("cpu_input_boost_wq_mstu", WQ_HIGHPRI, 0);
	if (!b->wq_mstu) {
		ret = -ENOMEM;
		return ret;
	}

	b->wq_gpu = alloc_workqueue("cpu_input_boost_wq_gpu", WQ_HIGHPRI, 0);
	if (!b->wq_gpu) {
		ret = -ENOMEM;
		return ret;
	}

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	b->cpu_notif.priority = INT_MAX;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX-2;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	boost_thread = kthread_run_low_power(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(boost_thread)) {
		pr_err("Failed to start CPU boost thread, err: %ld\n",
		       PTR_ERR(boost_thread));
		goto unregister_drm_notif;
	}
	
	b->gpu_device = kgsl_get_device(KGSL_DEVICE_3D0);
	if (IS_ERR_OR_NULL(b->gpu_device))
		return 0;
	b->gpu_pwr = &b->gpu_device->pwrctrl;
	return 0;

unregister_drm_notif:
	msm_drm_unregister_client(&b->msm_drm_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
late_initcall(cpu_input_boost_init);
