#ifndef __PWM_VICTORY_DATA_H_
#define __PWM_VICTORY_DATA_H_

struct s3c_pwm_pdata {
	unsigned int gpio_no;
	const char *gpio_name;
	unsigned int gpio_set_value;
};

static struct s3c_pwm_pdata victory_pwm_data[] = {
	{
		.gpio_no = S5PV210_GPD0(0),
		.gpio_name = "GPD",
		.gpio_set_value = S5PV210_GPD_0_0_TOUT_0,
	}, {
#ifdef CONFIG_MACH_VICTORY
		.gpio_no = 0,
		.gpio_name = NULL,
		.gpio_set_value = 0,
#else
		.gpio_no = S5PV210_GPD0(1),
		.gpio_name = "GPD",
		.gpio_set_value = S5PV210_GPD_0_1_TOUT_1,
#endif
	}, {
		.gpio_no = S5PV210_GPD0(2),
		.gpio_name = "GPD",
		.gpio_set_value = S5PV210_GPD_0_2_TOUT_2,
	}, {
		.gpio_no = S5PV210_GPD0(3),
		.gpio_name = "GPD",
		.gpio_set_value = S5PV210_GPD_0_3_TOUT_3,
	}, {
		.gpio_no = 0,
		.gpio_name = NULL,
		.gpio_set_value = 0,
	},
};

#endif
