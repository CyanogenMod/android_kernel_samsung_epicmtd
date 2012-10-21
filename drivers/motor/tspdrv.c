/*
** =========================================================================
** File:
**     tspdrv.c
**
** Description:
**     TouchSense Kernel Module main entry-point.
**
** Portions Copyright (c) 2008-2009 Immersion Corporation. All Rights Reserved.
**
** This file contains Original Code and/or Modifications of Original Code
** as defined in and that are subject to the GNU Public License v2 -
** (the 'License'). You may not use this file except in compliance with the
** License. You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA or contact
** TouchSenseSales@immersion.com.
**
** The Original Code and all software distributed under the License are
** distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
** EXPRESS OR IMPLIED, AND IMMERSION HEREBY DISCLAIMS ALL SUCH WARRANTIES,
** INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS
** FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see
** the License for the specific language governing rights and limitations
** under the License.
** =========================================================================
*/

#ifndef __KERNEL__
#define __KERNEL__
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/vibrator-pwm.h>
#include <asm/uaccess.h>
#include <linux/hrtimer.h>
#include "../staging/android/timed_output.h"
#include <linux/delay.h>
#include <linux/gpio.h>
//#include <linux/wakelock.h>
#include <linux/regulator/consumer.h>

#include "tspdrv.h"
#include "ImmVibeSPI.c"

/* Device name and version information */
#define VERSION_STR " v3.3.13.0\n"                  /* DO NOT CHANGE - this is auto-generated */
#define VERSION_STR_LEN 16                          /* account extra space for future extra digits in version number */
static char g_szDeviceName[  (VIBE_MAX_DEVICE_NAME_LENGTH
                            + VERSION_STR_LEN)
                            * NUM_ACTUATORS];       /* initialized in init_module */
static size_t g_cchDeviceName;                      /* initialized in init_module */

//static struct wake_lock vib_wake_lock;

/* Flag indicating whether the driver is in use */
static char g_bIsPlaying = false;

/* Buffer to store data sent to SPI */
#define SPI_BUFFER_SIZE (NUM_ACTUATORS * (VIBE_OUTPUT_SAMPLE_SIZE + SPI_HEADER_SIZE))
static int g_bStopRequested = false;
static actuator_samples_buffer g_SamplesBuffer[NUM_ACTUATORS] = {{0}};

/* For QA purposes */
#ifdef QA_TEST
#define FORCE_LOG_BUFFER_SIZE   128
#define TIME_INCREMENT          5
static int g_nTime = 0;
static int g_nForceLogIndex = 0;
static VibeInt8 g_nForceLog[FORCE_LOG_BUFFER_SIZE];
#endif

#define IMPLEMENT_AS_CHAR_DRIVER
#ifdef IMPLEMENT_AS_CHAR_DRIVER
static int g_nMajor = 0;
#endif

/* Needs to be included after the global variables because it uses them */
#include "VibeOSKernelLinuxTime.c"

/* timed_output */
#define PWM_PERIOD	43542
#define PWM_DUTY_MAX	43500
#define PWM_DUTY_MIN	21750

static unsigned int pwm_duty		= 100;
static unsigned int pwm_duty_value	= PWM_DUTY_MAX;
static unsigned int multiplier		= (PWM_DUTY_MAX - PWM_DUTY_MIN) / 100;

#define MAX_TIMEOUT		10000 /* 10s */
static int max_timeout = MAX_TIMEOUT;
static int pwm_value = 0;

static struct hrtimer timer;
struct vibrator_platform_data vib_plat_data;

#ifdef CONFIG_MACH_FORTE
extern void set_ldo12_reg(int);
#endif

static int set_vibetonz(int timeout)
{
	VibeDebug("%s: timeout: %d \n", __func__, timeout);
	//gpio_request(GPIO_VIBTONE_EN1, "GPIO_VIBTONE_EN1");
	if(!timeout) {
		pwm_disable(Immvib_pwm);
		VibeDebug("DISABLE\n");
		gpio_set_value(vib_plat_data.vib_enable_gpio, 0);
#if defined(CONFIG_MACH_FORTE)
		set_ldo12_reg(0);
#endif
		gpio_direction_input(vib_plat_data.vib_enable_gpio);
		s3c_gpio_setpull(vib_plat_data.vib_enable_gpio, S3C_GPIO_PULL_DOWN);
	} else {
		VibeDebug("%s: vibeDuty: %d | vibePeriod: %d \n", __func__, pwm_duty_value, PWM_PERIOD);
		pwm_config(Immvib_pwm, pwm_duty_value, PWM_PERIOD);
		pwm_enable(Immvib_pwm);
		VibeDebug("ENABLE\n");
		gpio_direction_output(vib_plat_data.vib_enable_gpio, 0);
#if defined(CONFIG_MACH_FORTE)
		set_ldo12_reg(1);
#endif
		mdelay(1);
		gpio_set_value(vib_plat_data.vib_enable_gpio, 1);
	}
	//gpio_free(GPIO_VIBTONE_EN1);
	return 0;
}


static enum hrtimer_restart vibetonz_timer_func(struct hrtimer *timer)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	set_vibetonz(0);
	return HRTIMER_NORESTART;
}

static int get_time_for_vibetonz(struct timed_output_dev *dev)
{
	int remaining = 0;
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	if (hrtimer_active(&timer)) {
		ktime_t r = hrtimer_get_remaining(&timer);
		struct timeval t = ktime_to_timeval(r);
		remaining = t.tv_sec * 1000 + t.tv_usec / 1000;
	} else
		remaining = 0;
	return remaining;
}

static void enable_vibetonz_from_user(struct timed_output_dev *dev, int value)
{
	VibeDebug("%s : time = %d msec \n", __func__, value);
	hrtimer_cancel(&timer);
	set_vibetonz(value);
	if (value > 0)
	{
		if (value > max_timeout)
			value = max_timeout;
		hrtimer_start(&timer,
						ktime_set(value / 1000, (value % 1000) * 1000000),
						HRTIMER_MODE_REL);
	}
}

static struct timed_output_dev timed_output_vt = {
	.name     = "vibrator",
	.get_time = get_time_for_vibetonz,
	.enable   = enable_vibetonz_from_user,
};

static void vibetonz_start(void)
{
	int ret = 0;
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer.function = vibetonz_timer_func;
	ret = timed_output_dev_register(&timed_output_vt);
	if(ret)
		printk(KERN_ERR "[VibeTonz] timed_output_dev_register is fail \n");
}

static ssize_t victory_vibrator_set_duty(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	sscanf(buf, "%d\n", &pwm_duty);

	if (pwm_duty >= 0 && pwm_duty <= 100)
		pwm_duty_value = (pwm_duty * multiplier) + PWM_DUTY_MIN;

	return size;
}
static ssize_t victory_vibrator_show_duty(struct device *dev,
					struct device_attribute *attr,
					const char *buf)
{
	return sprintf(buf, "%d", pwm_duty);
}
static DEVICE_ATTR(pwm_duty, S_IRUGO | S_IWUGO, victory_vibrator_show_duty, victory_vibrator_set_duty);
static struct attribute *pwm_duty_attributes[] = {
	&dev_attr_pwm_duty,
	NULL
};
static struct attribute_group pwm_duty_group = {
	.attrs = pwm_duty_attributes,
};
static struct miscdevice pwm_duty_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pwm_duty",
};

/* File IO */
static int open(struct inode *inode, struct file *file);
static int release(struct inode *inode, struct file *file);
static ssize_t read(struct file *file, char *buf, size_t count, loff_t *ppos);
static ssize_t write(struct file *file, const char *buf, size_t count, loff_t *ppos);
static long ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static struct file_operations fops =
{
    .owner =    THIS_MODULE,
    .read =     read,
    .write =    write,
    .unlocked_ioctl = ioctl,
    .open =     open,
    .release =  release
};

#ifndef IMPLEMENT_AS_CHAR_DRIVER
static struct miscdevice miscdev =
{
	.minor =    MISC_DYNAMIC_MINOR,
	.name =     MODULE_NAME,
	.fops =     &fops
};
#endif

static int suspend(struct platform_device *pdev, pm_message_t state);
static int resume(struct platform_device *pdev);

#ifdef VIBE_TUNING
struct class *vibetonz_class;
EXPORT_SYMBOL(vibetonz_class);

struct device *vibeTest_test;
EXPORT_SYMBOL(vibeTest_test);

extern long int freq_count;
static ssize_t vibeTimer_show(struct device *dev,
						struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "[VibeTonz] example: echo 2000 >vibeTimer \n");
}
static ssize_t vibeTimer_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	unsigned long val;
	int res;
	if ((res = strict_strtoul(buf, 10, &val)) < 0)
		return res;
	VibeDebug("vibeTimer value:%lu \n", val);
	if (val > 0) {
		VibeDebug("enable \n");
		enable_vibetonz_from_user((struct timed_output_dev *)dev, (int)val);
	} else {
		VibeDebug("disable \n");
		enable_vibetonz_from_user((struct timed_output_dev *)dev, (int)val);
	}
	return count;
}
static DEVICE_ATTR(vibeTimer, S_IRUGO | S_IWUSR, vibeTimer_show, vibeTimer_store);

static ssize_t vibeForce_show(struct device *dev,
						struct device_attribute *attr, char *buf)
{
	/*
	 * TW force adjustment steps { start, most, end }
	 *  1st (off):    none
	 *  2nd (faint):  {  42,  18, -125 }
	 *  3rd (light):  {  65,  38, -125 }
	 *  4th (medium): {  97,  70, -125 }
	 *  5th (high):   { 112,  98, -125 }
	 *  6th (full):   { 123, 121, -125 }
	 */
	return snprintf(buf, PAGE_SIZE, "[VibeTonz] example: echo 121 >vibeForce \n");
}
static ssize_t vibeForce_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	unsigned long val;
	int res;
	if ((res = strict_strtoul(buf, 10, &val)) < 0)
		return res;
	VibeDebug("vibeForce value:%lu \n", val);
	if (val > 0) {
		VibeDebug("enable \n");
		ImmVibeSPI_ForceOut_Set(0, val);
	} else {
		VibeDebug("disable \n");
		ImmVibeSPI_ForceOut_AmpDisable(val);
	}
	return count;
}
static DEVICE_ATTR(vibeForce, S_IRUGO | S_IWUSR, vibeForce_show, vibeForce_store);
#endif /* VIBE_TUNING */

static int vibrator_probe(struct platform_device *pdev)
{
	struct vibrator_platform_data *pdata = pdev->dev.platform_data;
	int i, nRet = 0; //, ret = 0;
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	vib_plat_data.timer_id = pdata->timer_id;
	vib_plat_data.vib_enable_gpio = pdata->vib_enable_gpio;
	if (gpio_is_valid(vib_plat_data.vib_enable_gpio)) {
		if ((nRet = gpio_request(vib_plat_data.vib_enable_gpio, "GPIO_VIBTONE_EN1")))
			printk(KERN_ERR "Failed to request GPIO_VIBTONE_EN1! %d \n",nRet);
	}
#ifdef CONFIG_MACH_FORTE
	vib_plat_data.regulator = regulator_get(&pdev->dev, "vibrator");
	if (!vib_plat_data.regulator) {
		printk("failed to get vibrator regulator\n");
	}
	ret = regulator_enable(vib_plat_data.regulator);
	if (ret < 0) {
		printk("fialed to enable vibrator regulator\n");
	}
#endif
	ImmVibeSPI_ForceOut_Initialize();
	VibeOSKernelLinuxInitTimer();
	/* Get and concatenate device name and initialize data buffer */
	g_cchDeviceName = 0;
	for (i=0; i<NUM_ACTUATORS; i++)
	{
		char *szName = g_szDeviceName + g_cchDeviceName;
		ImmVibeSPI_Device_GetName(i, szName, VIBE_MAX_DEVICE_NAME_LENGTH);
		/* Append version information and get buffer length */
		strcat(szName, VERSION_STR);
		g_cchDeviceName += strlen(szName);

		g_SamplesBuffer[i].nIndexPlayingBuffer = -1; /* Not playing */
		g_SamplesBuffer[i].actuatorSamples[0].nBufferSize = 0;
		g_SamplesBuffer[i].actuatorSamples[1].nBufferSize = 0;
	}
	//wake_lock_init(&vib_wake_lock, WAKE_LOCK_SUSPEND, "vib_present");

	if (misc_register(&pwm_duty_device)) {
		printk("%s misc_register(pwm_duty) failed\n", __FUNCTION__);
	} else {
		if (sysfs_create_group(&pwm_duty_device.this_device->kobj, &pwm_duty_group)) {
			printk("failed to create sysfs group for device pwm_duty\n");
		}
	}

#ifdef VIBE_TUNING
	// ---------- class creation at '/sys/class/vibetonz/'------------------------------
	vibetonz_class = class_create(THIS_MODULE, "vibetonz");
	if (IS_ERR(vibetonz_class))
		pr_err("Failed to create class(vibetonz)!\n");
	// ---------- device creation at '/sys/class/vibetonz/vibeTest/'------------------------------
	vibeTest_test = device_create(vibetonz_class, NULL, 0, NULL, "vibeTest");
	if (IS_ERR(vibeTest_test))
		pr_err("Failed to create device(vibeTest)!\n");
	// ---------- file creation at '/sys/class/vibetonz/vibeTest/vibeTimer'------------------------------
	if (device_create_file(vibeTest_test, &dev_attr_vibeTimer) < 0)
		pr_err("Failed to create device file(%s)!\n", dev_attr_vibeTimer.attr.name);
	// ---------- file creation at '/sys/class/vibetonz/vibeTest/vibeForce'------------------------------
	if (device_create_file(vibeTest_test, &dev_attr_vibeForce) < 0)
		pr_err("Failed to create device file(%s)!\n", dev_attr_vibeForce.attr.name);
#endif
	vibetonz_start();
	return 0;
}

static int vibrator_remove(struct platform_device *pdev)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
#ifdef CONFIG_MACH_FORTE
	regulator_disable(vib_plat_data.regulator);
#endif
	DbgOut((KERN_INFO "tspdrv: vibrator removed .\n"));
	return 0;
}

static struct platform_driver vibrator_driver = {
	.probe 		= vibrator_probe,
	.remove 	= vibrator_remove,
	.suspend 	= suspend,
	.resume 	= resume,
	.driver 	= {
		.name  = "cm4040_cs",
		.owner = THIS_MODULE,
	},
};

/* Module info */
MODULE_AUTHOR("Immersion Corporation");
MODULE_DESCRIPTION("TouchSense Kernel Module");
MODULE_LICENSE("GPL v2");

int init_module(void)
{
	int nRet;//, i;   /* initialized below */
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	DbgOut((KERN_INFO "tspdrv: init_module.\n"));

#ifdef IMPLEMENT_AS_CHAR_DRIVER
	g_nMajor = register_chrdev(0, MODULE_NAME, &fops);
	if (g_nMajor < 0) {
		DbgOut((KERN_ERR "tspdrv: can't get major number.\n"));
		return g_nMajor;
	}
#else
	nRet = misc_register(&miscdev);
	if (nRet) {
		DbgOut((KERN_ERR "tspdrv: misc_register failed.\n"));
		return nRet;
	}
#endif
	nRet = platform_driver_register(&vibrator_driver);
	if (nRet) {
		DbgOut((KERN_ERR "tspdrv: platform_driver_register failed.\n"));
	}
	return nRet;
}

void cleanup_module(void)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	DbgOut((KERN_INFO "tspdrv: cleanup_module.\n"));
	VibeOSKernelLinuxTerminateTimer();
	ImmVibeSPI_ForceOut_Terminate();
	platform_driver_unregister(&vibrator_driver);
#ifdef IMPLEMENT_AS_CHAR_DRIVER
	unregister_chrdev(g_nMajor, MODULE_NAME);
#else
	misc_deregister(&miscdev);
#endif
	gpio_free(vib_plat_data.vib_enable_gpio);
}

static int open(struct inode *inode, struct file *file)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	DbgOut((KERN_INFO "tspdrv: open.\n"));
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	return 0;
}

static int release(struct inode *inode, struct file *file)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	DbgOut((KERN_INFO "tspdrv: release.\n"));
	/*
	** Reset force and stop timer when the driver is closed, to make sure
	** no dangling semaphore remains in the system, especially when the
	** driver is run outside of immvibed for testing purposes.
	*/
	VibeOSKernelLinuxStopTimer();
	/*
	** Clear the variable used to store the magic number to prevent
	** unauthorized caller to write data. TouchSense service is the only
	** valid caller.
	*/
	file->private_data = (void*)NULL;
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	const size_t nBufSize = (g_cchDeviceName > (size_t)(*ppos)) ? min(count, g_cchDeviceName - (size_t)(*ppos)) : 0;
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	/* End of buffer, exit */
	if (0 == nBufSize)
		return 0;
	if (0 != copy_to_user(buf, g_szDeviceName + (*ppos), nBufSize)) {
		/* Failed to copy all the data, exit */
		DbgOut((KERN_ERR "tspdrv: copy_to_user failed.\n"));
		return 0;
	}
	/* Update file position and return copied buffer size */
	*ppos += nBufSize;
	return nBufSize;
}

static ssize_t write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	int i = 0;
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	*ppos = 0;  /* file position not used, always set to 0 */
	/*
	** Prevent unauthorized caller to write data.
	** TouchSense service is the only valid caller.
	*/
	if (file->private_data != (void*)TSPDRV_MAGIC_NUMBER) {
		DbgOut((KERN_ERR "tspdrv: unauthorized write.\n"));
		return 0;
	}
	/* Check buffer size */
	if ((count <= SPI_HEADER_SIZE) || (count >= SPI_BUFFER_SIZE)) {
		DbgOut((KERN_ERR "tspdrv: invalid write buffer size.\n"));
		return 0;
	}
	while (i < count) {
		int nIndexFreeBuffer;   /* initialized below */
		samples_buffer* pInputBuffer = (samples_buffer*)(&buf[i]);
		if ((i + SPI_HEADER_SIZE) >= count) {
			/*
			** Index is about to go beyond the buffer size.
			** (Should never happen).
			*/
			DbgOut((KERN_EMERG "tspdrv: invalid buffer index.\n"));
		}
		/* Check bit depth */
		if (8 != pInputBuffer->nBitDepth) {
			DbgOut((KERN_WARNING "tspdrv: invalid bit depth. Use default value (8).\n"));
		}
		/* The above code not valid if SPI header size is not 3 */
#if (SPI_HEADER_SIZE != 3)
#error "SPI_HEADER_SIZE expected to be 3"
#endif
		/* Check buffer size */
		if ((i + SPI_HEADER_SIZE + pInputBuffer->nBufferSize) > count) {
			/*
			** Index is about to go beyond the buffer size.
			** (Should never happen).
			*/
			DbgOut((KERN_EMERG "tspdrv: invalid data size.\n"));
		}
		/* Check actuator index */
		if (NUM_ACTUATORS <= pInputBuffer->nActuatorIndex) {
			DbgOut((KERN_ERR "tspdrv: invalid actuator index.\n"));
			i += (SPI_HEADER_SIZE + pInputBuffer->nBufferSize);
			continue;
		}
		if (0 == g_SamplesBuffer[pInputBuffer->nActuatorIndex].actuatorSamples[0].nBufferSize) {
			nIndexFreeBuffer = 0;
		} else if (0 == g_SamplesBuffer[pInputBuffer->nActuatorIndex].actuatorSamples[1].nBufferSize) {
			nIndexFreeBuffer = 1;
		} else {
			/* No room to store new samples  */
			DbgOut((KERN_ERR "tspdrv: no room to store new samples.\n"));
			return 0;
		}
		/* Store the data in the actuator's free buffer */
		if (0 != copy_from_user(&(g_SamplesBuffer[pInputBuffer->nActuatorIndex].actuatorSamples[nIndexFreeBuffer]),
								&(buf[i]), (SPI_HEADER_SIZE + pInputBuffer->nBufferSize))) {
			/* Failed to copy all the data, exit */
			DbgOut((KERN_ERR "tspdrv: copy_from_user failed.\n"));
			return 0;
		}
		/* if the no buffer is playing, prepare to play g_SamplesBuffer[pInputBuffer->nActuatorIndex].actuatorSamples[nIndexFreeBuffer] */
		if ( -1 == g_SamplesBuffer[pInputBuffer->nActuatorIndex].nIndexPlayingBuffer) {
			g_SamplesBuffer[pInputBuffer->nActuatorIndex].nIndexPlayingBuffer = nIndexFreeBuffer;
			g_SamplesBuffer[pInputBuffer->nActuatorIndex].nIndexOutputValue = 0;
		}
		/* Call SPI */
		// NOTE: ImmVibeSPI_ForceOut_SetSamples() does NOTHING!
		ImmVibeSPI_ForceOut_SetSamples(pInputBuffer->nActuatorIndex,
								pInputBuffer->nBitDepth, pInputBuffer->nBufferSize,
								&(g_SamplesBuffer[pInputBuffer->nActuatorIndex].actuatorSamples[nIndexFreeBuffer].dataBuffer[0]));
		/* Increment buffer index */
		i += (SPI_HEADER_SIZE + pInputBuffer->nBufferSize);
	}
#ifdef QA_TEST
	g_nForceLog[g_nForceLogIndex++] = g_cSPIBuffer[0];
	if (g_nForceLogIndex >= FORCE_LOG_BUFFER_SIZE) {
		for (i=0; i<FORCE_LOG_BUFFER_SIZE; i++) {
			printk("<6> [VibeTonz] %d\t%d\n", g_nTime, g_nForceLog[i]);
			g_nTime += TIME_INCREMENT;
		}
		g_nForceLogIndex = 0;
	}
#endif
	/* Start the timer after receiving new output force */
	g_bIsPlaying = true;
	VibeOSKernelLinuxStartTimer();
	return count;
}

static long ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
#ifdef QA_TEST
	int i;
#endif
	VibeDebug("%s : cmd: %u \n", __func__, cmd);
	switch (cmd) {
		case TSPDRV_STOP_KERNEL_TIMER:
			VibeDebug("%s @ STOP_KERNEL_TIMER: cmd: %u \n", __func__, cmd);
			/*
			** As we send one sample ahead of time, we need to finish playing the last sample
			** before stopping the timer. So we just set a flag here.
			*/
			if (true == g_bIsPlaying)
				g_bStopRequested = true;
#ifdef QA_TEST
			if (g_nForceLogIndex) {
				for (i=0; i<g_nForceLogIndex; i++) {
					printk("<6>%d\t%d\n", g_nTime, g_nForceLog[i]);
					g_nTime += TIME_INCREMENT;
				}
			}
			g_nTime = 0;
			g_nForceLogIndex = 0;
#endif
			break;
		case TSPDRV_IDENTIFY_CALLER:
			VibeDebug("%s @ IDENTIFY_CALLER: cmd: %u \n", __func__, cmd);
			if (TSPDRV_MAGIC_NUMBER == arg)
				file->private_data = (void*)TSPDRV_MAGIC_NUMBER;
			break;
		case TSPDRV_ENABLE_AMP:
			VibeDebug("%s @ ENABLE_AMP: cmd: %u \n", __func__, cmd);
			//wake_lock(&vib_wake_lock);
			ImmVibeSPI_ForceOut_AmpEnable(arg);
			break;
		case TSPDRV_DISABLE_AMP:
			VibeDebug("%s @ DISABLE_AMP: cmd: %u \n", __func__, cmd);
			ImmVibeSPI_ForceOut_AmpDisable(arg);
			//wake_unlock(&vib_wake_lock);
			break;
		case TSPDRV_GET_NUM_ACTUATORS:
			VibeDebug("%s @ GET_NUM_ACTUATORS: cmd: %u \n", __func__, cmd);
			return NUM_ACTUATORS;
	}
	return 0;
}

static int suspend(struct platform_device *pdev, pm_message_t state)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	if (g_bIsPlaying) {
		DbgOut((KERN_INFO "tspdrv: can't suspend, still playing effects.\n"));
		return -EBUSY;
	} else {
#ifdef CONFIG_MACH_FORTE
		regulator_disable(vib_plat_data.regulator);
#endif
		DbgOut((KERN_INFO "tspdrv: suspend.\n"));
		return 0;
	}
}

static int resume(struct platform_device *pdev)
{
	VibeDebug("%s: enter: %i \n", __func__, __LINE__);
	DbgOut((KERN_INFO "tspdrv: resume.\n"));
#ifdef CONFIG_MACH_FORTE
	regulator_enable(vib_plat_data.regulator);
#endif
	return 0;   /* can resume */
}

