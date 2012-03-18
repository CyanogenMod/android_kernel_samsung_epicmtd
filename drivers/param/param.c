#include <linux/kernel.h>
#include <linux/module.h>
#include <mach/param.h>

static void get_param_value(int idx, void *value);
static void set_param_value(int idx, void *value);

static int switch_sel  = 5; /* USB_SAMSUNG_KIES_MASK | USB_SEL_MASK */
static int debug_level = 0;
static int reboot_mode = REBOOT_MODE_NONE;

static int __init param_init(void) {
	sec_get_param_value = get_param_value;
	sec_set_param_value = set_param_value;

	return 0;
}

static void get_param_value(int idx, void *value) {
	switch (idx) {
	case __SWITCH_SEL:
		*(int *)value = switch_sel;
		break;
	case __PHONE_DEBUG_ON:
		*(int *)value = debug_level;
		break;
	case __REBOOT_MODE:
		*(int *)value = reboot_mode;
		break;
	default:
		pr_warning("param:%s: Unknown idx: %d.\n", __func__, idx);
		break;
	}
}

static void set_param_value(int idx, void *value) {
	switch (idx) {
	case __SWITCH_SEL:
		switch_sel = *(int *)value;
		break;
	case __PHONE_DEBUG_ON:
		debug_level = *(int *)value;
		break;
	case __REBOOT_MODE:
		reboot_mode = *(int *)value;
		break;
	default:
		pr_warning("param:%s: Unknown idx: %d.\n", __func__, idx);
		break;
	}
}

module_init(param_init);
MODULE_LICENSE("GPL");
