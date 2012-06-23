/* linux/arch/arm/plat-samsung/dev-pwm.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Copyright (c) 2007 Ben Dooks
 * Copyright (c) 2008 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>, <ben-linux@fluff.org>
 *
 * S3C series device definition for the PWM timer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <mach/irqs.h>

#ifdef CONFIG_MACH_VICTORY
#include <mach/gpio.h>
#include <mach/gpio-bank.h>
#include <mach/pwm-victory-data.h>
#endif

#include <plat/devs.h>

#define TIMER_RESOURCE_SIZE (1)

#define TIMER_RESOURCE(_tmr, _irq)			\
	(struct resource [TIMER_RESOURCE_SIZE]) {	\
		[0] = {					\
			.start	= _irq,			\
			.end	= _irq,			\
			.flags	= IORESOURCE_IRQ	\
		}					\
	}

#define DEFINE_S3C_TIMER(_tmr_no, _irq)			\
	.name		= "s3c24xx-pwm",		\
	.id		= _tmr_no,			\
	.num_resources	= TIMER_RESOURCE_SIZE,		\
	.resource	= TIMER_RESOURCE(_tmr_no, _irq),	\

#define DEFINE_VICTORY_TIMER(_tmr_no, _irq, _plat_data) \
	.name = "s3c24xx-pwm", \
	.id = _tmr_no, \
	.num_resources = TIMER_RESOURCE_SIZE, \
	.resource = TIMER_RESOURCE(_tmr_no, _irq), \
	.dev = { \
		.platform_data = _plat_data, \
	}

/*
 * since we already have an static mapping for the timer,
 * we do not bother setting any IO resource for the base.
 */
#ifdef CONFIG_MACH_VICTORY
struct platform_device s3c_device_timer[] = {
	[0] = { DEFINE_VICTORY_TIMER(0, IRQ_TIMER0, &victory_pwm_data[0]) },
	[1] = { DEFINE_VICTORY_TIMER(1, IRQ_TIMER1, &victory_pwm_data[1]) },
	[2] = { DEFINE_VICTORY_TIMER(2, IRQ_TIMER2, &victory_pwm_data[2]) },
	[3] = { DEFINE_VICTORY_TIMER(3, IRQ_TIMER3, &victory_pwm_data[3]) },
	[4] = { DEFINE_VICTORY_TIMER(4, IRQ_TIMER4, &victory_pwm_data[4]) },
};
#else
struct platform_device s3c_device_timer[] = {
	[0] = { DEFINE_S3C_TIMER(0, IRQ_TIMER0) },
	[1] = { DEFINE_S3C_TIMER(1, IRQ_TIMER1) },
	[2] = { DEFINE_S3C_TIMER(2, IRQ_TIMER2) },
	[3] = { DEFINE_S3C_TIMER(3, IRQ_TIMER3) },
	[4] = { DEFINE_S3C_TIMER(4, IRQ_TIMER4) },
};
#endif
EXPORT_SYMBOL(s3c_device_timer);
