// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "devfreq_boost: " fmt

#include <linux/cpu.h>
#include <linux/devfreq_boost.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/msm_drm_notify.h>
#include <linux/version.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned short flex_boost_duration __read_mostly = CONFIG_FLEX_DEVFREQ_BOOST_DURATION_MS;
static unsigned short input_boost_duration __read_mostly = CONFIG_DEVFREQ_INPUT_BOOST_DURATION_MS;
static unsigned int devfreq_thread_prio __read_mostly = CONFIG_DEVFREQ_THREAD_PRIORITY;

module_param(flex_boost_duration, short, 0644);
module_param(input_boost_duration, short, 0644);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	FLEX_BOOST,
	WAKE_BOOST,
	MAX_BOOST
};

struct boost_dev {
	struct kthread_worker boost_worker;
	struct task_struct *boost_worker_thread;
	struct devfreq *df;
	struct kthread_work input_boost;
	struct kthread_delayed_work input_unboost;
	struct kthread_work flex_boost;
	struct kthread_delayed_work flex_unboost;
	struct kthread_work max_boost;
	struct kthread_delayed_work max_unboost;
	unsigned int max_boost_jiffies;
	unsigned int wake_boost_jiffies;
	unsigned long state;
	unsigned long boost_freq;

};

struct df_boost_drv {
	struct boost_dev devices[DEVFREQ_MAX];
	struct notifier_block msm_drm_notif;
	atomic_t screen_awake;
};

static struct df_boost_drv *df_boost_drv_g __read_mostly;

static void devfreq_update_boosts(struct boost_dev *b, unsigned long state)
{
	struct devfreq *df = b->df;

	mutex_lock(&df->lock);
	if (test_bit(SCREEN_OFF, &state)) {
		df->min_freq = df->profile->freq_table[0];
		df->max_boost = test_bit(WAKE_BOOST, &state) ? 
						true :
						false;
	} else {
		df->min_freq = test_bit(INPUT_BOOST, &state) || test_bit(FLEX_BOOST, &state) ?
			       min(b->boost_freq, df->max_freq) :
			       df->profile->freq_table[0];
		df->max_boost = test_bit(MAX_BOOST, &state) || test_bit(WAKE_BOOST, &state);
	}
	update_devfreq(df);
	mutex_unlock(&df->lock);
}

void devfreq_boost_kick_flex(enum df_device device)
{
	struct df_boost_drv *d = df_boost_drv_g;
	struct boost_dev *b = d->devices + device;

	if (!READ_ONCE(b->df) || test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(FLEX_BOOST, &b->state);
	kthread_queue_work(&b->boost_worker, &b->flex_boost);
}

static void devfreq_flex_boost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), flex_boost);
	
	devfreq_update_boosts(b, b->state);
	kthread_mod_delayed_work(&b->boost_worker, &b->flex_unboost, msecs_to_jiffies(flex_boost_duration));
}

static void devfreq_flex_unboost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), flex_unboost.work);

	clear_bit(FLEX_BOOST, &b->state);
	devfreq_update_boosts(b, b->state);
}

void devfreq_boost_kick_max(enum df_device device, unsigned int duration_ms)
{
	struct df_boost_drv *d = df_boost_drv_g;
	struct boost_dev *b = d->devices + device;

	if (!READ_ONCE(b->df) || test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(MAX_BOOST, &b->state);

	b->max_boost_jiffies = msecs_to_jiffies(duration_ms);
	kthread_queue_work(&b->boost_worker, &b->max_boost);
}

static void devfreq_max_boost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), max_boost);
	
	devfreq_update_boosts(b, b->state);
	kthread_mod_delayed_work(&b->boost_worker, &b->max_unboost, b->max_boost_jiffies);
}

static void devfreq_max_unboost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), max_unboost.work);

	clear_bit(WAKE_BOOST, &b->state);
	clear_bit(MAX_BOOST, &b->state);
	devfreq_update_boosts(b, b->state);
}

void devfreq_boost_kick_wake(enum df_device device, unsigned int duration_ms)
{
	struct df_boost_drv *d = df_boost_drv_g;
	struct boost_dev *b = d->devices + device;

	if (!READ_ONCE(b->df) || !test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(WAKE_BOOST, &b->state);

	b->wake_boost_jiffies = msecs_to_jiffies(duration_ms);
	kthread_queue_work(&b->boost_worker, &b->max_boost);
}

void devfreq_boost_kick(struct boost_dev *b)
{
	if (!READ_ONCE(b->df) || test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(INPUT_BOOST, &b->state);
	kthread_queue_work(&b->boost_worker, &b->input_boost);
}

static void devfreq_input_boost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), input_boost);
	
	devfreq_update_boosts(b, b->state);
	kthread_mod_delayed_work(&b->boost_worker, &b->input_unboost, msecs_to_jiffies(input_boost_duration));
}

static void devfreq_input_unboost(struct kthread_work *work)
{
	struct boost_dev *b = container_of(work, typeof(*b), input_unboost.work);

	clear_bit(INPUT_BOOST, &b->state);
	devfreq_update_boosts(b, b->state);
}

void devfreq_register_boost_device(enum df_device device, struct devfreq *df)
{
	struct df_boost_drv *d = df_boost_drv_g;
	struct boost_dev *b;

	df->is_boost_device = true;
	b = d->devices + device;
	WRITE_ONCE(b->df, df);
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct df_boost_drv *d = container_of(nb, typeof(*d), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int i;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	for (i = 0; i < DEVFREQ_MAX; i++) {
		struct boost_dev *b = d->devices + i;

		if (*blank == MSM_DRM_BLANK_UNBLANK_CUST) {
			devfreq_boost_kick_wake(DEVFREQ_MSM_CPUBW, 500);
			clear_bit(SCREEN_OFF, &b->state);
		} else {
			set_bit(SCREEN_OFF, &b->state);
			devfreq_update_boosts(b, b->state);
		}
	}

	return NOTIFY_OK;
}

static void devfreq_boost_input_event(struct input_handle *handle,
				      unsigned int type, unsigned int code,
				      int value)
{
	struct df_boost_drv *d = handle->handler->private;
	int i;

	for (i = 0; i < DEVFREQ_MAX; i++)
		devfreq_boost_kick(d->devices + i);
}

static int devfreq_boost_input_connect(struct input_handler *handler,
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
	handle->name = "devfreq_boost_handle";

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

static void devfreq_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id devfreq_boost_ids[] = {
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

static struct input_handler devfreq_boost_input_handler = {
	.event		= devfreq_boost_input_event,
	.connect	= devfreq_boost_input_connect,
	.disconnect	= devfreq_boost_input_disconnect,
	.name		= "devfreq_boost_handler",
	.id_table	= devfreq_boost_ids
};

static int __init devfreq_boost_init(void)
{
	static struct sched_param sched_max_rt_prio;
	struct df_boost_drv *d;
	int i, ret;

	if (devfreq_thread_prio == 99)
 		sched_max_rt_prio.sched_priority = MAX_RT_PRIO - 1;
	else 
		sched_max_rt_prio.sched_priority = devfreq_thread_prio;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	for (i = 0; i < DEVFREQ_MAX; i++) {
		struct boost_dev *b = d->devices + i;	
		b->state = 0;
		clear_bit(SCREEN_OFF, &b->state);
		kthread_init_worker(&b->boost_worker);
		b->boost_worker_thread = kthread_run_low_power(kthread_worker_fn, &b->boost_worker,
				       "def_freq_boost_thread_%d",i);
		if (IS_ERR(b->boost_worker_thread)) {
			ret = PTR_ERR(b->boost_worker_thread);
			pr_err("Failed to start kworker, err: %d\n", ret);
			kthread_destroy_worker(&b->boost_worker);
		}

		ret = sched_setscheduler(b->boost_worker_thread, SCHED_FIFO, &sched_max_rt_prio);
		if (ret)
			pr_err("Failed to set SCHED_FIFO on kworker, err: %d\n", ret);

		kthread_init_work(&b->input_boost, devfreq_input_boost);
		kthread_init_delayed_work(&b->input_unboost, devfreq_input_unboost);
		kthread_init_work(&b->flex_boost, devfreq_flex_boost);
		kthread_init_delayed_work(&b->flex_unboost, devfreq_flex_unboost);
		kthread_init_work(&b->max_boost, devfreq_max_boost);
		kthread_init_delayed_work(&b->max_unboost, devfreq_max_unboost);
	}

	d->devices[DEVFREQ_MSM_CPUBW].boost_freq =
		CONFIG_DEVFREQ_MSM_CPUBW_BOOST_FREQ;

	devfreq_boost_input_handler.private = d;
	ret = input_register_handler(&devfreq_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
	}

	d->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	d->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&d->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register dsi_panel_notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	df_boost_drv_g = d;

	return 0;

unregister_handler:
	input_unregister_handler(&devfreq_boost_input_handler);
}
subsys_initcall(devfreq_boost_init);
