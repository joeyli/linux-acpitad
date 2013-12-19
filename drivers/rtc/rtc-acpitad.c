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

/*
 * returns day of the year [0-365]
 */
static inline int
compute_yday(struct acpi_time *acpit)
{
	/* acpi_time.month is in the [1-12] so, we need -1 */
	return rtc_year_days(acpit->day, acpit->month - 1, acpit->year);
}

/*
 * returns day of the week [0-6] 0=Sunday
 */
static int
compute_wday(struct acpi_time *acpit)
{
	if (acpit->year < 1900) {
		pr_err("ACPI year < 1900, invalid date\n");
		return -1;
	}

	return rtc_wday(acpit->day, acpit->month - 1, acpit->year);
}

static void
convert_to_acpi_time(struct rtc_time *tm, struct acpi_time *acpit)
{
	acpit->year	    = tm->tm_year + 1900;
	acpit->month        = tm->tm_mon + 1;
	acpit->day          = tm->tm_mday;
	acpit->hour         = tm->tm_hour;
	acpit->minute       = tm->tm_min;
	acpit->second       = tm->tm_sec;
	acpit->milliseconds = 0;
	acpit->daylight	    = tm->tm_isdst ? ACPI_ISDST : 0;
}

static void
convert_from_acpi_time(struct acpi_time *acpit, struct rtc_time *tm)
{
	memset(tm, 0, sizeof(*tm));
	tm->tm_sec	= acpit->second;
	tm->tm_min	= acpit->minute;
	tm->tm_hour	= acpit->hour;
	tm->tm_mday	= acpit->day;
	tm->tm_mon	= acpit->month - 1;
	tm->tm_year	= acpit->year - 1900;

	/* day of the week [0-6], Sunday=0 */
	tm->tm_wday = compute_wday(acpit);

	/* day in the year [1-365]*/
	tm->tm_yday = compute_yday(acpit);

	switch (acpit->daylight & ACPI_ISDST) {
	case ACPI_ISDST:
		tm->tm_isdst = 1;
		break;
	case ACPI_TIME_AFFECTED_BY_DAYLIGHT:
		tm->tm_isdst = 0;
		break;
	default:
		tm->tm_isdst = -1;
	}
}

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
	struct acpi_time *acpit;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	ret = acpi_read_time(acpit);
	if (ret)
		return ret;

	convert_from_acpi_time(acpit, tm);

	return rtc_valid_tm(tm);
}

static int acpitad_set_time(struct device *dev, struct rtc_time *tm)
{
	struct acpi_time *acpit;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	/* read current timzone to avoid overwrite it by set time */
	ret = acpi_read_time(acpit);
	if (ret)
		goto error_read;

	convert_to_acpi_time(tm, acpit);

	ret = acpi_set_time(acpit);

error_read:
	kfree(acpit);
	return ret;
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
