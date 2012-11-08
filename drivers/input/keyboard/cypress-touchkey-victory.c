/*
 * Copyright 2006-2010, Cypress Semiconductor Corporation.
 * Copyright (C) 2010, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor
 * Boston, MA  02110-1301, USA.
 *
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/earlysuspend.h>
#include <linux/input/cypress-touchkey.h>
#include <linux/regulator/max8893.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#if defined CONFIG_MACH_VICTORY
#include <mach/gpio.h>
#include <mach/gpio-victory.h>
#endif

#define SCANCODE_MASK		0x07
#define UPDOWN_EVENT_MASK	0x08
#define ESD_STATE_MASK		0x10

#define BACKLIGHT_ON		0x1
#define BACKLIGHT_OFF		0x2

#define OLD_BACKLIGHT_ON	0x1
#define OLD_BACKLIGHT_OFF	0x2

#define DEVICE_NAME "cypress-touchkey"

#ifdef CONFIG_MACH_VICTORY
#define BACKLIGHT_DELAYS 1
#endif

struct cypress_touchkey_devdata {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct touchkey_platform_data *pdata;
	struct early_suspend early_suspend;
	u8 backlight_on;
	u8 backlight_off;
	bool is_dead;
	bool is_powering_on;
	bool has_legacy_keycode;
	bool is_delay_led_on;
	bool is_backlight_on;
	bool is_key_pressed;
	bool is_bl_disabled;
	struct mutex mutex;
#ifdef BACKLIGHT_DELAYS
	struct delayed_work key_off_work;
#endif
};

struct cypress_touchkey_devdata *devdata_led;

/* Backlight key-off delay:
 *
 * Milliseconds to wait between when a key is released and when the
 * backlight is turned off.  Used to turn the backlight off following a key
 * press when the "buttons" light is already disabled in liblight.  Since
 * the backlight turns on automatically when a key is pressed (released),
 * in absence of liblight/framework intervention, the backlight will stay
 * on until explicitly turned off.
 *
 * Behavior of values:
 *   0 - 112: Backlight stays on indefinitely (the backlight take long
 *            enough to turn on that the explicit off has no effect).
 * 114 - 133: The non-pressed keys briefly light, the pressed key is dark.
 * 134 - inf: The non-pressed keys light first, then the pressed key
 *            lights, until the backlight is turned off. */
#ifdef BACKLIGHT_DELAYS
static       unsigned int key_off_delay     =  750;
static const unsigned int key_off_delay_max = 1000;
#endif

/* Backlight late-resume on/off delay:
 *
 * Milliseconds to wait between when the controller is powered-on in
 * late-resume and the backlight is turned on (or off, if it's on by
 * default).  On victory, the controller is powered-on during qt602240's
 * late-resume, so no delay is necessary. */
#ifdef BACKLIGHT_DELAYS
static       unsigned int resume_delay     =    0;
static const unsigned int resume_delay_max = 1000;
#else
static const unsigned int resume_delay     =   80;
#endif

static int i2c_touchkey_read_byte(struct cypress_touchkey_devdata *devdata,
					u8 *val)
{
	int ret;
	int retry = 2;

	while (true) {
		ret = i2c_smbus_read_byte(devdata->client);
		if (ret >= 0) {
			*val = ret;
			return 0;
		}

		dev_err(&devdata->client->dev, "i2c read error\n");
		if (!retry--)
			break;
		msleep(10);
	}

	return ret;
}

static int i2c_touchkey_write(struct cypress_touchkey_devdata *devdata, 
						u8 * val, unsigned int len)
{
	int err;
	struct i2c_msg msg[1];
	unsigned char data[2];
	int retry = 2;

	while (retry--) {
		data[0] = *val;
		msg->addr = devdata->client->addr;
		msg->flags = 0;
		msg->len = len;
		msg->buf = data;
		err = i2c_transfer(devdata->client->adapter, msg, 1);
		if (err >= 0)
			return 0;
		printk(KERN_DEBUG "%s %d i2c transfer error\n", __func__,
		       __LINE__);
		msleep(10);
	}
	return err;
}

static int i2c_touchkey_write_byte(struct cypress_touchkey_devdata *devdata,
					u8 val)
{
	int ret;
	int retry = 2;

	while (true) {
		ret = i2c_smbus_write_byte(devdata->client, val);
		if (!ret)
			return 0;

		dev_err(&devdata->client->dev, "i2c write error\n");
		if (!retry--)
			break;
		msleep(10);
	}

	return ret;
}

static ssize_t touch_led_control(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	int ret;

	mutex_lock(&devdata_led->mutex);

	if (strncmp(buf, "1", 1) == 0)
	{
		if (devdata_led->is_bl_disabled)
			goto unlock;

		devdata_led->is_backlight_on = true;
		if (devdata_led->is_powering_on || devdata_led->is_key_pressed) {
			dev_err(dev, "%s: delay led on \n", __func__);
			devdata_led->is_delay_led_on = true;
			goto unlock;
		}
		ret = i2c_touchkey_write(devdata_led, &devdata_led->backlight_on, 1);
		printk("Touch Key led ON\n");
	}
	else
	{
		devdata_led->is_backlight_on = false;
		if (devdata_led->is_powering_on || devdata_led->is_key_pressed) {
			dev_err(dev, "%s: delay led off\n", __func__);
			devdata_led->is_delay_led_on = true;
			goto unlock;
		}
		ret = i2c_touchkey_write(devdata_led, &devdata_led->backlight_off, 1);
		printk("Touch Key led OFF\n");			
	}

	if (ret)
		dev_err(dev, "%s: touchkey led i2c failed\n", __func__);

unlock:
	mutex_unlock(&devdata_led->mutex);
	return size;
}

static ssize_t touch_control_enable_disable(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	printk("touchkey_enable =1 , disable=0 %c \n", *buf);

	mutex_lock(&devdata_led->mutex);

	if (strncmp(buf, "0", 1) == 0){
		devdata_led->is_powering_on = true;
		disable_irq_nosync(devdata_led->client->irq);
#if defined CONFIG_MACH_VICTORY
		gpio_direction_output(_3_GPIO_TOUCH_EN, TOUCHKEY_OFF);
#else		
		if(devdata_led->pdata->touchkey_onoff)
			devdata_led->pdata->touchkey_onoff(TOUCHKEY_OFF);
#endif	
}
	else if(strncmp(buf, "1", 1) == 0){
#if 0
		devdata_led->is_powering_on = false;

		if(devdata_led->pdata->touchkey_onoff)
		devdata_led->pdata->touchkey_onoff(TOUCHKEY_ON);

		enable_irq(devdata->client->irq);
#endif
	}
	else
		printk("touchkey_enable_disable: unknown command %c \n", *buf);

	mutex_unlock(&devdata_led->mutex);
	return size;
}

/* Mutex must be locked when calling. */
static void all_keys_up(struct cypress_touchkey_devdata *devdata)
{
	int i;

	for (i = 0; i < devdata->pdata->keycode_cnt; i++)
		input_report_key(devdata->input_dev,
						devdata->pdata->keycode[i], 0);

	input_sync(devdata->input_dev);
	devdata->is_key_pressed = false;
}

/* Mutex must be locked when calling. */
static int recovery_routine(struct cypress_touchkey_devdata *devdata)
{
	int ret = -1;
	int retry = 10;
	u8 data;
	int irq_eint;

	if (unlikely(devdata->is_dead)) {
		dev_err(&devdata->client->dev, "%s: Device is already dead, "
				"skipping recovery\n", __func__);
		return -ENODEV;
	}

	irq_eint = devdata->client->irq;

	all_keys_up(devdata);

	disable_irq_nosync(irq_eint);
	while (retry--) {
	if(devdata->pdata->touchkey_onoff) {
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);
		devdata->pdata->touchkey_onoff(TOUCHKEY_ON);
	}
		ret = i2c_touchkey_read_byte(devdata, &data);
		if (!ret) {
			enable_irq(irq_eint);
			goto out;
		}
		dev_err(&devdata->client->dev, "%s: i2c transfer error retry = "
				"%d\n", __func__, retry);
	}
	devdata->is_dead = true;
	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);
	dev_err(&devdata->client->dev, "%s: touchkey died\n", __func__);
out:
	return ret;
}

extern unsigned int touch_state_val;
extern void TSP_forced_release(void);

/* Turns off backlight following a key release.
 *
 * Note: there's technically a race here if a second key is pressed "just"
 * as the first scheduled work fires but has yet to execute.  If so, then
 * this function will turn off the backlight early, only to immediately be
 * turned on again.  Oh well. */
#ifdef BACKLIGHT_DELAYS
static void key_off_func(struct work_struct *work)
{
	struct cypress_touchkey_devdata *devdata = container_of(work,
	       struct cypress_touchkey_devdata, key_off_work.work);

	mutex_lock(&devdata->mutex);

	/* Check if "buttons" light was turned on while waiting. */
	if (!devdata->is_powering_on && !devdata->is_backlight_on)
		i2c_touchkey_write(devdata, &devdata->backlight_off, 1);

	mutex_unlock(&devdata->mutex);
}
#endif

static irqreturn_t touchkey_interrupt_thread(int irq, void *touchkey_devdata)
{
	u8 data;
	int i;
	int ret;
	int scancode;
	struct cypress_touchkey_devdata *devdata = touchkey_devdata;

	mutex_lock(&devdata->mutex);

	if (devdata->is_powering_on)
		goto unlock;

	ret = i2c_touchkey_read_byte(devdata, &data);
	if (ret || (data & ESD_STATE_MASK)) {
		ret = recovery_routine(devdata);
		if (ret) {
			dev_err(&devdata->client->dev, "%s: touchkey recovery "
					"failed!\n", __func__);
			goto err;
		}
	}
/*
	if (devdata->has_legacy_keycode) {
		scancode = (data & SCANCODE_MASK) - 1;
		if (scancode < 0 || scancode >= devdata->pdata->keycode_cnt) {
			dev_err(&devdata->client->dev, "%s: scancode is out of "
				"range\n", __func__);
			goto err;
		}
		input_report_key(devdata->input_dev,
			devdata->pdata->keycode[scancode],
			!(data & UPDOWN_EVENT_MASK));
                printk("Key code A %d\n", devdata->pdata->keycode[scancode]);
	} else {
		for (i = 0; i < devdata->pdata->keycode_cnt; i++)
			input_report_key(devdata->input_dev,
				devdata->pdata->keycode[i],
				!!(data & (1U << i))); */

		
	if (data & UPDOWN_EVENT_MASK) {
		scancode = (data & SCANCODE_MASK) - 1;
		input_report_key(devdata->input_dev,
			devdata->pdata->keycode[scancode], 0);
		input_sync(devdata->input_dev);
		devdata->is_key_pressed = false;
		dev_dbg(&devdata->client->dev, "[release] cypress touch key : %d \n",
			devdata->pdata->keycode[scancode]);
		printk("Touch_key=release\n");

#ifndef BACKLIGHT_DELAYS
		if (devdata->is_delay_led_on) {
			/* A request to turn on/off the backlight came while a key was pressed and
			 * was deferred.  Process it now, except on victory where the backlight
			 * turns on automatically shortly after release, and turning it off now has
			 * no effect. */
			i2c_touchkey_write(devdata, devdata->is_backlight_on ?
			                   &devdata->backlight_on : &devdata->backlight_off, 1);
		}
#endif
		devdata->is_delay_led_on = false;

#ifdef BACKLIGHT_DELAYS
		if (!devdata->is_backlight_on) {
			/* "buttons" light is (likely) disabled, must explicitly turn off backlight
			 * since it turns on automatically during a key release. */
			cancel_delayed_work(&devdata->key_off_work);
			schedule_delayed_work(&devdata->key_off_work,
			                      msecs_to_jiffies(key_off_delay));
		}
#endif
	} else {
		if (!touch_state_val) {	
			if (devdata->has_legacy_keycode) {
				scancode = (data & SCANCODE_MASK) - 1;
				if (scancode < 0 || scancode >= devdata->pdata->keycode_cnt) {
					dev_err(&devdata->client->dev, "%s: scancode is out of "
						"range\n", __func__);
					goto err;
				}
				if (scancode == 1)
					TSP_forced_release();
				input_report_key(devdata->input_dev,
					devdata->pdata->keycode[scancode], 1);
				devdata->is_key_pressed = true;
				
				dev_dbg(&devdata->client->dev, "[press] cypress touch key : %d \n",
					devdata->pdata->keycode[scancode]);	
				//printk("Touch_key=%d\n",devdata->pdata->keycode[scancode]);
					printk("Touch_key=press\n");
				} else {
				for (i = 0; i < devdata->pdata->keycode_cnt; i++)
				input_report_key(devdata->input_dev,
					devdata->pdata->keycode[i],
					!!(data & (1U << i)));        
				//printk("Key code B %d\n", devdata->pdata->keycode[scancode]);
			}		

		input_sync(devdata->input_dev);
		}
	
	}	
err:
unlock:
	mutex_unlock(&devdata->mutex);
	return IRQ_HANDLED;
}

static irqreturn_t touchkey_interrupt_handler(int irq, void *touchkey_devdata)
{
	struct cypress_touchkey_devdata *devdata = touchkey_devdata;

	/* Can't lock the mutex in interrupt context, but should be OK. */
	if (devdata->is_powering_on) {
		dev_dbg(&devdata->client->dev, "%s: ignoring spurious boot "
					"interrupt\n", __func__);
		return IRQ_HANDLED;
	}

	return IRQ_WAKE_THREAD;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void cypress_touchkey_early_suspend(struct early_suspend *h)
{
	struct cypress_touchkey_devdata *devdata =
		container_of(h, struct cypress_touchkey_devdata, early_suspend);

	mutex_lock(&devdata->mutex);

	devdata->is_powering_on = true;

	if (unlikely(devdata->is_dead))
		goto unlock;

	disable_irq_nosync(devdata->client->irq);

	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);

	all_keys_up(devdata);

#ifdef BACKLIGHT_DELAYS
	cancel_delayed_work(&devdata->key_off_work);
#endif

unlock:
	mutex_unlock(&devdata->mutex);
}

static void cypress_touchkey_early_resume(struct early_suspend *h)
{
	struct cypress_touchkey_devdata *devdata =
		container_of(h, struct cypress_touchkey_devdata, early_suspend);
	
#ifndef CONFIG_MACH_VICTORY
	msleep(1);
#endif

	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_ON);

	#if 0
	if (i2c_touchkey_write_byte(devdata, devdata->backlight_on)) {
		devdata->is_dead = true;
	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);
		dev_err(&devdata->client->dev, "%s: touch keypad not responding"
				" to commands, disabling\n", __func__);
		return;
	}
	#endif

	mutex_lock(&devdata->mutex);

	devdata->is_dead = false;
	enable_irq(devdata->client->irq);
	devdata->is_powering_on = false;

	if (resume_delay > 0) {
		mutex_unlock(&devdata->mutex);
		msleep(resume_delay); // touch power on time
		mutex_lock(&devdata->mutex);
	}

	if (devdata->is_delay_led_on){
		int ret;
		if (devdata->is_backlight_on) {
			ret = i2c_touchkey_write(devdata, &devdata->backlight_on, 1);
			dev_err(&devdata->client->dev,"%s: Touch Key led ON ret = %d\n",__func__, ret);
		} else {
			ret = i2c_touchkey_write(devdata, &devdata->backlight_off, 1);
			dev_err(&devdata->client->dev,"%s: Touch Key led OFF ret = %d\n",__func__, ret);
		}
	}
	devdata->is_delay_led_on = false;

	mutex_unlock(&devdata->mutex);
}
#endif

static DEVICE_ATTR(brightness, 0660, NULL,touch_led_control);
static DEVICE_ATTR(enable_disable, 0660, NULL,touch_control_enable_disable);

#ifdef BACKLIGHT_DELAYS
#define DELAY_ATTR(delay) \
static ssize_t delay##_show(struct device *dev, \
                            struct device_attribute *attr, char *buf) \
{ \
	return snprintf(buf, PAGE_SIZE, "%u\n", delay); \
} \
\
static ssize_t delay##_store(struct device *dev, \
                             struct device_attribute *attr, \
                             const char *buf, size_t count) \
{ \
	unsigned long val; \
	int res; \
\
	if ((res = strict_strtoul(buf, 10, &val)) < 0) \
		return res; \
\
	if (val > delay##_max) \
		return -EINVAL; \
\
	delay = val; \
\
	return count; \
} \
\
static DEVICE_ATTR(delay, S_IRUGO | S_IWUSR, delay##_show, delay##_store);

DELAY_ATTR(key_off_delay)
DELAY_ATTR(resume_delay)
#undef DELAY_ATTR
#endif

static ssize_t touchleds_disabled_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
	int res;

	mutex_lock(&devdata_led->mutex);
	res = snprintf(buf, PAGE_SIZE, "%u\n",
	               (unsigned int)devdata_led->is_bl_disabled);
	mutex_unlock(&devdata_led->mutex);

	return res;
}

static ssize_t touchleds_disabled_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
	unsigned long val;
	int res;

	if ((res = strict_strtoul(buf, 10, &val)) < 0)
		return res;

	mutex_lock(&devdata_led->mutex);
	devdata_led->is_bl_disabled = val;
	mutex_unlock(&devdata_led->mutex);

	return count;
}

static ssize_t touchleds_voltage_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
	int res;

	mutex_lock(&devdata_led->mutex);
	res = snprintf(buf, PAGE_SIZE, "%u\n",
	               (unsigned int)touchkey_voltage);
	mutex_unlock(&devdata_led->mutex);

	return res;
}

static ssize_t touchleds_voltage_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
	unsigned long val, previous_val;
	int res;

	if ((res = strict_strtoul(buf, 10, &val)) < 0)
		return res;

	if ((val > TOUCHKEY_VOLTAGE_MAX) || (val < TOUCHKEY_VOLTAGE_MIN))
		return -EINVAL;

	previous_val = touchkey_voltage;

	mutex_lock(&devdata_led->mutex);

	touchkey_voltage = val;

	/* If we set a voltage higher than the current voltage,
	 * the controller interprets it as a touch interrupt and
	 * without a drop in voltage will not issue a touch release.
	 * Disable the controller and re-enable after setting the
	 * new voltage. */
	if (val > previous_val) {
		/* Initiate a mock suspend */
		disable_irq_nosync(devdata_led->client->irq);
		gpio_direction_output(_3_GPIO_TOUCH_EN, TOUCHKEY_OFF);
		all_keys_up(devdata_led);
#ifdef BACKLIGHT_DELAYS
		cancel_delayed_work(&devdata_led->key_off_work);
#endif

		/* Set new TOUCHKEY voltage */
		max8893_ldo_set_voltage_direct(MAX8893_LDO2, val, val);
		msleep(1);

		/* Re-enable the controller */
		gpio_direction_output(_3_GPIO_TOUCH_EN, TOUCHKEY_ON);
		enable_irq(devdata_led->client->irq);
	} else {
		max8893_ldo_set_voltage_direct(MAX8893_LDO2, val, val);
	}

	mutex_unlock(&devdata_led->mutex);

	return count;
}

static DEVICE_ATTR(touchleds_disabled, S_IRUGO | S_IWUSR,
                   touchleds_disabled_show, touchleds_disabled_store);
static DEVICE_ATTR(touchleds_voltage, S_IRUGO | S_IWUSR,
                   touchleds_voltage_show, touchleds_voltage_store);

extern struct class *sec_class;
struct device *ts_key_dev;

static int cypress_touchkey_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct input_dev *input_dev;
	struct cypress_touchkey_devdata *devdata;
	u8 data[3];
	int err;
	int cnt;

	if (!dev->platform_data) {
		dev_err(dev, "%s: Platform data is NULL\n", __func__);
		return -EINVAL;
	}

	devdata = kzalloc(sizeof(*devdata), GFP_KERNEL);
	devdata_led = devdata;
	if (devdata == NULL) {
		dev_err(dev, "%s: failed to create our state\n", __func__);
		return -ENODEV;
	}

	devdata->client = client;
	i2c_set_clientdata(client, devdata);

	devdata->pdata = client->dev.platform_data;
	if (!devdata->pdata->keycode) {
		dev_err(dev, "%s: Invalid platform data\n", __func__);
		err = -EINVAL;
		goto err_null_keycodes;
	}

	strlcpy(devdata->client->name, DEVICE_NAME, I2C_NAME_SIZE);

	input_dev = input_allocate_device();
	if (!input_dev) {
		err = -ENOMEM;
		goto err_input_alloc_dev;
	}

	devdata->input_dev = input_dev;
	dev_set_drvdata(&input_dev->dev, devdata);
	input_dev->name = DEVICE_NAME;
	input_dev->id.bustype = BUS_HOST;

	for (cnt = 0; cnt < devdata->pdata->keycode_cnt; cnt++)
		input_set_capability(input_dev, EV_KEY,
					devdata->pdata->keycode[cnt]);

	err = input_register_device(input_dev);
	if (err)
		goto err_input_reg_dev;

	devdata->is_powering_on = true;
	devdata->is_delay_led_on = false;
	devdata->is_backlight_on = true;
	devdata->is_key_pressed = false;
	devdata->is_bl_disabled = false;
	mutex_init(&devdata->mutex);
#ifdef BACKLIGHT_DELAYS
	INIT_DELAYED_WORK(&devdata->key_off_work, key_off_func);
#endif

	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_ON);	

	err = i2c_master_recv(client, data, sizeof(data));
	if (err < sizeof(data)) {
		if (err >= 0)
			err = -EIO;
		dev_err(dev, "%s: error reading hardware version\n", __func__);
		goto err_read;
	}

	dev_info(dev, "%s: hardware rev1 = %#02x, rev2 = %#02x\n", __func__,
				data[1], data[2]);
	if (data[1] < 0xc4 && (data[1] >= 0x8 ||
				(data[1] == 0x8 && data[2] >= 0x9))) {
		devdata->backlight_on = BACKLIGHT_ON;
		devdata->backlight_off = BACKLIGHT_OFF;
	} else {
		devdata->backlight_on = OLD_BACKLIGHT_ON;
		devdata->backlight_off = OLD_BACKLIGHT_OFF;
	}

	/* devdata->has_legacy_keycode = data[1] >= 0xc4 || data[1] < 0x9 || */
	/* 				(data[1] == 0x9 && data[2] < 0x9); */
	devdata->has_legacy_keycode = 1;

	if (sec_class == NULL)
		   sec_class = class_create(THIS_MODULE, "sec");
	
		   if (IS_ERR(sec_class))
				   pr_err("Failed to create class(sec)!\n");
	
	   ts_key_dev = device_create(sec_class, NULL, 0, NULL, "t_key");
	   if (IS_ERR(ts_key_dev))
		   pr_err("Failed to create device(ts)!\n");
	   if (device_create_file(ts_key_dev, &dev_attr_brightness) < 0)
		   pr_err("Failed to create device file for Touch key!\n");
	    if (device_create_file(ts_key_dev, &dev_attr_enable_disable) < 0)
		pr_err("Failed to create device file for Touch key_enable_disable!\n");

#ifdef BACKLIGHT_DELAYS
	if (device_create_file(ts_key_dev, &dev_attr_key_off_delay) < 0)
		pr_err("Unable to create \"%s\".\n", dev_attr_key_off_delay.attr.name);

	if (device_create_file(ts_key_dev, &dev_attr_resume_delay) < 0)
		pr_err("Unable to create \"%s\".\n", dev_attr_resume_delay.attr.name);
#endif

	if (device_create_file(ts_key_dev, &dev_attr_touchleds_disabled) < 0)
		pr_err("Unable to create \"%s\".\n", dev_attr_touchleds_disabled.attr.name);

	if (device_create_file(ts_key_dev, &dev_attr_touchleds_voltage) < 0)
		pr_err("Unable to create \"%s\".\n", dev_attr_touchleds_voltage.attr.name);

#if 0
	err = i2c_touchkey_write_byte(devdata, devdata->backlight_on);
	if (err) {
		dev_err(dev, "%s: touch keypad backlight on failed\n",
				__func__);
		goto err_backlight_on;
	}
#endif
	if (request_threaded_irq(client->irq, touchkey_interrupt_handler,
				touchkey_interrupt_thread, IRQF_TRIGGER_FALLING,
				DEVICE_NAME, devdata)) {
		dev_err(dev, "%s: Can't allocate irq.\n", __func__);
		goto err_req_irq;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	devdata->early_suspend.suspend = cypress_touchkey_early_suspend;
	devdata->early_suspend.resume = cypress_touchkey_early_resume;
#endif
	register_early_suspend(&devdata->early_suspend);

	mutex_lock(&devdata->mutex);
	devdata->is_powering_on = false;
	mutex_unlock(&devdata->mutex);

	return 0;

err_req_irq:
err_backlight_on:
err_read:
	
	if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);
#ifdef BACKLIGHT_DELAYS
	device_remove_file(ts_key_dev, &dev_attr_key_off_delay);
	device_remove_file(ts_key_dev, &dev_attr_resume_delay);
#endif
	device_remove_file(ts_key_dev, &dev_attr_touchleds_disabled);
	device_remove_file(ts_key_dev, &dev_attr_touchleds_voltage);
	mutex_destroy(&devdata->mutex);
	input_unregister_device(input_dev);
	goto err_input_alloc_dev;
err_input_reg_dev:
	input_free_device(input_dev);
err_input_alloc_dev:
err_null_keycodes:
	kfree(devdata);
	return err;
}

static int __devexit i2c_touchkey_remove(struct i2c_client *client)
{
	struct cypress_touchkey_devdata *devdata = i2c_get_clientdata(client);

	unregister_early_suspend(&devdata->early_suspend);
	/* If the device is dead IRQs are disabled, we need to rebalance them */
	if (unlikely(devdata->is_dead))
		enable_irq(client->irq);
	else if(devdata->pdata->touchkey_onoff)
		devdata->pdata->touchkey_onoff(TOUCHKEY_OFF);
	free_irq(client->irq, devdata);
	mutex_lock(&devdata->mutex);
	all_keys_up(devdata);
	mutex_unlock(&devdata->mutex);
#ifdef BACKLIGHT_DELAYS
	device_remove_file(ts_key_dev, &dev_attr_key_off_delay);
	device_remove_file(ts_key_dev, &dev_attr_resume_delay);
	cancel_delayed_work_sync(&devdata->key_off_work);
#endif
	device_remove_file(ts_key_dev, &dev_attr_touchleds_disabled);
	device_remove_file(ts_key_dev, &dev_attr_touchleds_voltage);
	mutex_destroy(&devdata->mutex);
	input_unregister_device(devdata->input_dev);
	kfree(devdata);
	return 0;
}

static const struct i2c_device_id cypress_touchkey_id[] = {
	{ CYPRESS_TOUCHKEY_DEV_NAME, 0 },
};

MODULE_DEVICE_TABLE(i2c, cypress_touchkey_id);

struct i2c_driver touchkey_i2c_driver = {
	.driver = {
		.name = "cypress_touchkey_driver",
	},
	.id_table = cypress_touchkey_id,
	.probe = cypress_touchkey_probe,
	.remove = __devexit_p(i2c_touchkey_remove),
};

static int __init touchkey_init(void)
{
	int ret = 0;

	ret = i2c_add_driver(&touchkey_i2c_driver);
	if (ret)
		pr_err("%s: cypress touch keypad registration failed. (%d)\n",
				__func__, ret);

	return ret;
}

static void __exit touchkey_exit(void)
{
	i2c_del_driver(&touchkey_i2c_driver);
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@@@");
MODULE_DESCRIPTION("cypress touch keypad");
