/* drivers/misc/max17048_tweaks.c
 *
 * Copyright 2020  wlkmanist
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/max17048_tweaks.h>
#include <linux/stat.h>
#include <linux/export.h>

static int max_voltage_mv = 0;

static ssize_t max17048_max_voltage_mv_read(struct device * dev,
            struct device_attribute * attr, char * buf) {
    return sprintf(buf, "%u\n", max_voltage_mv);
}

static ssize_t max17048_max_voltage_mv_write(struct device * dev,
            struct device_attribute * attr, const char * buf, size_t size) {
    int data;

    if(sscanf(buf, "%u\n", &data) == 1) {
	    set_max_voltage_mv(data);
	} else {
	    pr_info("%s: Invalid input\n", __FUNCTION__);
	}

    return size;
}

static DEVICE_ATTR(max_voltage_mv, 0644, max17048_max_voltage_mv_read,
                max17048_max_voltage_mv_write);

static struct attribute *max17048_tweaks_attributes[] = 
    {
	&dev_attr_max_voltage_mv.attr,
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

int get_max_voltage_mv(void) /// use this to replace max_voltage_mv
{
    return max_voltage_mv;
}
EXPORT_SYMBOL(get_max_voltage_mv);

void set_max_voltage_mv(int data)
{
 	    if (data >= VBT_MIN_MV && data <= VBT_MAX_MV) {
            max_voltage_mv = (data / 16 + ((data % 16) > 0)) * 16;
            pr_info("Battery max voltage set to %u\n", max_voltage_mv);
		} else {
		    pr_info("%s: Invalid input range %u\n", __FUNCTION__, data);
		}
}
EXPORT_SYMBOL(set_max_voltage_mv);

static int __init max17048_tweaks_init(void)
{

    /// add get max voltage value to init
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