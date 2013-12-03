/* A RTC driver for ACPI 5.0 Time and Alarm Device
 *
 * Copyright (C) 2013 SUSE Linux Products GmbH. All rights reserved.
 * Written by Lee, Chun-Yi (jlee@suse.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>

static int acpitad_read_gmtoff(struct device *dev, long int *arg)
{
	struct acpi_time *acpit;
	s16 timezone;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	ret = acpi_read_time(acpit);
	if (ret)
		goto error_read;

	/* transfer minutes to seconds east of UTC for userspace */
	timezone = (s16)le16_to_cpu(acpit->timezone);
	*arg = ACPI_UNSPECIFIED_TIMEZONE * 60;
	if (abs(timezone) != ACPI_UNSPECIFIED_TIMEZONE &&
	    abs(timezone) <= 1440)
		*arg = timezone * 60 * -1;

error_read:
	kfree(acpit);

	return ret;
}


static int acpitad_set_gmtoff(struct device *dev, long int arg)
{
	struct acpi_time *acpit;
	s16 timezone;
	int ret;

	/* transfer seconds east of UTC to minutes for ACPI */
	timezone = arg / 60 * -1;
	if (abs(timezone) > 1440 &&
	    abs(timezone) != ACPI_UNSPECIFIED_TIMEZONE)
		return -EINVAL;

	/* can not use -2047 */
	if (timezone == ACPI_UNSPECIFIED_TIMEZONE * -1)
		timezone = ACPI_UNSPECIFIED_TIMEZONE;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	ret = acpi_read_time(acpit);
	if (ret)
		goto error_read;

	acpit->timezone = (s16)cpu_to_le16(timezone);
	ret = acpi_set_time(acpit);

error_read:
	kfree(acpit);

	return ret;
}

static int acpitad_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	long int gmtoff;
	int err;

	switch (cmd) {
	case RTC_RD_GMTOFF:
		err = acpitad_read_gmtoff(dev, &gmtoff);
		if (err)
			return err;
		return put_user(gmtoff, (unsigned long __user *)arg);
	case RTC_SET_GMTOFF:
		return acpitad_set_gmtoff(dev, arg);
	default:
		return -ENOIOCTLCMD;
	}

	return 0;
}

static int acpitad_read_time(struct device *dev, struct rtc_time *tm)
{
	return acpi_get_rtc_time(tm);
}

static int acpitad_set_time(struct device *dev, struct rtc_time *tm)
{
	return acpi_set_rtc_time(tm);
}

static struct rtc_class_ops acpi_rtc_ops = {
	.ioctl          = acpitad_rtc_ioctl,
	.read_time      = acpitad_read_time,
	.set_time       = acpitad_set_time,
};

static int acpitad_rtc_probe(struct platform_device *dev)
{
	unsigned long cap;
	struct rtc_device *rtc;
	int ret;

	ret = acpi_tad_get_capability(&cap);
	if (ret)
		return ret;

	if (!(cap & TAD_CAP_GETSETTIME)) {
		acpi_rtc_ops.read_time = NULL;
		acpi_rtc_ops.set_time = NULL;
		pr_warn("No get/set time support\n");
	}

	/* ACPI Alarm at least need AC wake capability */
	if (!(cap & TAD_CAP_ACWAKE)) {
		acpi_rtc_ops.read_alarm = NULL;
		acpi_rtc_ops.set_alarm = NULL;
		pr_warn("No AC wake support\n");
	}

	/* register rtc device */
	rtc = rtc_device_register("rtc-acpitad", &dev->dev, &acpi_rtc_ops,
					THIS_MODULE);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	rtc->uie_unsupported = 1;
	rtc->caps = (RTC_TZ_CAP | RTC_DST_CAP);
	platform_set_drvdata(dev, rtc);

	return 0;
}

static int acpitad_rtc_remove(struct platform_device *dev)
{
	struct rtc_device *rtc = platform_get_drvdata(dev);

	rtc_device_unregister(rtc);

	return 0;
}

static struct platform_driver acpitad_rtc_driver = {
	.driver = {
		.name = "rtc-acpitad",
		.owner = THIS_MODULE,
	},
	.probe = acpitad_rtc_probe,
	.remove = acpitad_rtc_remove,
};

static int __init acpitad_rtc_init(void)
{
	return platform_driver_register(&acpitad_rtc_driver);
}

static void __exit acpitad_rtc_exit(void)
{
	platform_driver_unregister(&acpitad_rtc_driver);
}

module_init(acpitad_rtc_init);
module_exit(acpitad_rtc_exit);

MODULE_AUTHOR("Lee, Chun-Yi <jlee@suse.com>");
MODULE_DESCRIPTION("RTC ACPI Time and Alarm Device driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rtc-acpitad");
