/* drivers/misc/max17048_tweaks.c
 *
 * Copyright 2020  wlkmanist
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/max17048_tweaks.h>
#include <linux/stat.h>
#include <linux/export.h>
#include <linux/qpnp/qpnp-adc.h>

#ifdef CONFIG_STATE_NOTIFIER
#include <linux/state_notifier.h>
#endif

static int __read_mostly max_voltage_mv = 0;
static int full_soc = 0;
static int __read_mostly fcc_mah = 0;
int bat_voltage_now = 0;

#ifdef CONFIG_DYNAMIC_FSYNC
bool batt_soc_is_low = false;
#endif

static int __init get_def_max_voltage_mv(char *data)
{
    if (strcmp(data, "1") == 0) {
		set_max_voltage_mv(4200);         /* Li-ion  3.7V  */
	} else if (strcmp(data, "2") == 0) {
		set_max_voltage_mv(4350);         /* Li-poly 3.8V  */
	} else if (strcmp(data, "3") == 0) {
		set_max_voltage_mv(4400);         /* Li-poly 3.85V */
	} else {
		max_voltage_mv = 0;       /* Set from Device Tree  */
	}

	return 0;
}

__setup("bat_type=", get_def_max_voltage_mv);

static ssize_t max17048_max_voltage_mv_read(struct device * dev,
            struct device_attribute * attr, char * buf) {
    return sprintf(buf, "%u\n", max_voltage_mv);
}

static ssize_t max17048_max_voltage_mv_write(struct device * dev,
            struct device_attribute * attr, const char * buf, size_t size) {
    int data;

    if(sscanf(buf, "%u\n", &data) == 1 && data > 0) {
	    set_max_voltage_mv(data);
	} else {
	    pr_info("%s: Invalid input\n", __FUNCTION__);
	}

    return size;
}

static DEVICE_ATTR(max_voltage_mv, 0644, max17048_max_voltage_mv_read,
                max17048_max_voltage_mv_write);

static ssize_t max17048_fcc_mah_read(struct device * dev,
            struct device_attribute * attr, char * buf) {
    return sprintf(buf, "%u\n", fcc_mah);
}

static ssize_t max17048_fcc_mah_write(struct device * dev,
            struct device_attribute * attr, const char * buf, size_t size) {
    int data;

    if(sscanf(buf, "%u\n", &data) == 1) {
	    set_fcc_mah(data);
	} else {
	    pr_info("%s: Invalid input\n", __FUNCTION__);
	}

    return size;
}

static DEVICE_ATTR(fcc_mah, 0644, max17048_fcc_mah_read,
                max17048_fcc_mah_write);

int get_fcc_mah(void)
{
    return fcc_mah;
}
EXPORT_SYMBOL(get_fcc_mah);

void set_fcc_mah(int data)
{
 	if (data > 0) {
        fcc_mah = data;
	} else {
	    pr_info("%s: Invalid input range %u\n", __FUNCTION__, data);
	}
}
EXPORT_SYMBOL(set_fcc_mah);

static struct attribute *max17048_tweaks_attributes[] = 
    {
	&dev_attr_max_voltage_mv.attr,
    &dev_attr_fcc_mah.attr,
	NULL
    };

static struct attribute_group max17048_tweaks_group = 
    {
	.attrs  = max17048_tweaks_attributes,
    };

static struct miscdevice max17048_tweaks_device = 
    {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "max17048_tweaks",
    };

int get_max_voltage_mv(void)
{
    return max_voltage_mv;
}
EXPORT_SYMBOL(get_max_voltage_mv);

void set_max_voltage_mv(int data)
{
 	    if (data >= VBT_MIN_MV && data <= VBT_MAX_MV) {
            max_voltage_mv = (data / 16 + ((data % 16) > 0)) * 16;
            pr_info("Battery max voltage set to %u\n", max_voltage_mv);

                /* 970 - (4352 - max_voltage_mv) * 8 / 16 */
            full_soc = (max_voltage_mv - 2412) / 2;
            pr_info("full-soc set to %u\n", full_soc);
		} else {
		    pr_info("%s: Invalid input range %u\n", __FUNCTION__, data);
		}
}
EXPORT_SYMBOL(set_max_voltage_mv);

int get_full_soc(void)
{
    return full_soc;
}
EXPORT_SYMBOL(get_full_soc);

void set_full_soc(int full_soc_in)
{
    full_soc = full_soc_in;
}
EXPORT_SYMBOL(set_full_soc);

static int __init max17048_tweaks_init(void)
{
    int ret;

    pr_info("%s: misc_register(%s)\n", __FUNCTION__,
                    max17048_tweaks_device.name);

    ret = misc_register(&max17048_tweaks_device);
    if (ret) {
	    pr_err("%s: misc_register(%s) fail\n", __FUNCTION__,
                        max17048_tweaks_device.name);
	    return 1;
	}

    if (sysfs_create_group(&max17048_tweaks_device.this_device->kobj,
                    &max17048_tweaks_group) < 0) {
	    pr_err("%s: sysfs_create_group fail\n", __FUNCTION__);
	    pr_err("Failed to create sysfs group for device (%s)!\n",
                        max17048_tweaks_device.name);
	}

    return 0;
}

device_initcall(max17048_tweaks_init);