/* rtc.c - ACPI 5.0 Time and Alarm Driver
 *
 * Copyright (C) 2013 SUSE Linux Products GmbH. All rights reserved.
 * Written by Lee, Chun-Yi (jlee@suse.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <asm/time.h>

#define ACPI_TIME_ALARM_NAME		"Time and Alarm"
ACPI_MODULE_NAME(ACPI_TIME_ALARM_NAME);
#define ACPI_TIME_ALARM_CLASS		"time_alarm"

static const struct acpi_device_id time_alarm_ids[] = {
	{"ACPI000E", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, time_alarm_ids);

static struct platform_device rtc_acpitad_dev = {
	.name = "rtc-acpitad",
	.id = -1,
};

static acpi_handle acpi_tad_handle;
static unsigned long long cap;

/*
 * returns day of the year [0-365]
 */
static int
compute_acpi_yday(struct acpi_time *acpit)
{
	/* acpi_time.month is in the [1-12] so, we need -1 */
	return rtc_year_days(acpit->day, acpit->month - 1, acpit->year);
}

/*
 * returns day of the week [0-6] 0=Sunday
 */
static int
compute_acpi_wday(struct acpi_time *acpit)
{
	if (acpit->year < 1900) {
		pr_err("ACPI year %d < 1900, invalid date\n", acpit->year);
		return -1;
	}

	return rtc_wday(acpit->day, acpit->month - 1, acpit->year);
}

void convert_to_acpi_time(struct rtc_time *tm, struct acpi_time *acpit)
{
	acpit->year         = tm->tm_year + 1900;
	acpit->month        = tm->tm_mon + 1;
	acpit->day          = tm->tm_mday;
	acpit->hour         = tm->tm_hour;
	acpit->minute       = tm->tm_min;
	acpit->second       = tm->tm_sec;
	acpit->milliseconds = 0;
	acpit->daylight     = tm->tm_isdst ? ACPI_ISDST : 0;
}
EXPORT_SYMBOL(convert_to_acpi_time);

void convert_from_acpi_time(struct acpi_time *acpit, struct rtc_time *tm)
{
	memset(tm, 0, sizeof(*tm));
	tm->tm_sec      = acpit->second;
	tm->tm_min      = acpit->minute;
	tm->tm_hour     = acpit->hour;
	tm->tm_mday     = acpit->day;
	tm->tm_mon      = acpit->month - 1;
	tm->tm_year     = acpit->year - 1900;

	/* day of the week [0-6], Sunday=0 */
	tm->tm_wday = compute_acpi_wday(acpit);

	/* day in the year [1-365]*/
	tm->tm_yday = compute_acpi_yday(acpit);

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
EXPORT_SYMBOL(convert_from_acpi_time);

int acpi_read_time(struct acpi_time *output)
{
	unsigned long flags;
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	struct acpi_time *acpit;
	acpi_status status;

	if (!acpi_tad_handle) {
		pr_warn("acpi_tad_handle is empty\n");
		return -ENODEV;
	}

	if (!(cap & TAD_CAP_GETSETTIME))
		return -EINVAL;

	if (!output)
		return -EINVAL;

	spin_lock_irqsave(&rtc_lock, flags);
	status = acpi_evaluate_object(acpi_tad_handle, "_GRT", NULL, &result);
	spin_unlock_irqrestore(&rtc_lock, flags);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _GRT"));
		return -ENODEV;
	}

	obj = result.pointer;
	if (!obj ||
	    obj->type != ACPI_TYPE_BUFFER ||
	    obj->buffer.length > sizeof(struct acpi_time) ||
	    obj->buffer.length < offsetof(struct acpi_time, pad2)) {
		pr_err(ACPI_TIME_ALARM_NAME " Invalid _GRT data\n");
		return -EINVAL;
	}

	acpit = (struct acpi_time *) obj->buffer.pointer;
	if (acpit) {
		output->year = acpit->year;
		output->month = acpit->month;
		output->day = acpit->day;
		output->hour = acpit->hour;
		output->minute = acpit->minute;
		output->second = acpit->second;
		output->milliseconds = acpit->milliseconds;
		output->timezone = acpit->timezone;
		output->daylight = acpit->daylight;
	}

	return 0;
}
EXPORT_SYMBOL(acpi_read_time);

int acpi_get_rtc_time(struct rtc_time *tm)
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
EXPORT_SYMBOL(acpi_get_rtc_time);

int acpi_set_time(struct acpi_time *acpit)
{
	unsigned long flags;
	struct acpi_object_list input;
	union acpi_object params[1];
	unsigned long long output;
	acpi_status status;

	if (!acpi_tad_handle)
		return -ENODEV;

	if (!(cap & TAD_CAP_GETSETTIME))
		return -EINVAL;

	if (!acpit)
		return -EINVAL;

	input.count = 1;
	input.pointer = params;
	params[0].type = ACPI_TYPE_BUFFER;
	params[0].buffer.length = sizeof(struct acpi_time);
	params[0].buffer.pointer = (void *) acpit;

	spin_lock_irqsave(&rtc_lock, flags);
	status = acpi_evaluate_integer(acpi_tad_handle, "_SRT", &input, &output);
	spin_unlock_irqrestore(&rtc_lock, flags);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _SRT"));
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL(acpi_set_time);

int acpi_set_rtc_time(struct rtc_time *tm)
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
EXPORT_SYMBOL(acpi_set_rtc_time);

int acpi_tad_get_capability(unsigned long *output)
{
	if (!acpi_tad_handle)
		return -ENODEV;

	*output = cap;

	return 0;
}
EXPORT_SYMBOL(acpi_tad_get_capability);

static int acpi_time_alarm_add(struct acpi_device *device)
{
	if (!device)
		return -EINVAL;

	if (!acpi_tad_handle)
		return -EINVAL;

	if (platform_device_register(&rtc_acpitad_dev) < 0)
		pr_err("Unable to register rtc-acpitad device\n");

	return 0;
}

static struct acpi_driver acpi_time_alarm_driver = {
	.name = "time_and_alarm",
	.class = ACPI_TIME_ALARM_CLASS,
	.ids = time_alarm_ids,
	.ops = {
		.add = acpi_time_alarm_add,
		},
};

#if 0
static void acpi_get_time(struct timespec *now)
{
	struct acpi_time *acpit;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return;

	ret = acpi_read_time(acpit);
	if (ret) {
		/* should never happen */
		pr_err("acpi: can't read time.\n");
	} else {
		now->tv_sec = mktime(acpit->year, acpit->month, acpit->day,
				acpit->hour, acpit->minute, acpit->second);
		now->tv_nsec = 0;
	}
}

static int acpi_set_rtc_mmss(const struct timespec *now)
{
	unsigned long nowtime = now->tv_sec;
	struct rtc_time tm;
	struct acpi_time *acpit;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	ret = acpi_read_time(acpit);
	if (ret) {
		/* should never happen */
		pr_err("acpi: can't read time.\n");
		return -1;
	}

	rtc_time_to_tm(nowtime, &tm);
	if (!rtc_valid_tm(&tm)) {
		acpit->year = tm.tm_year + 1900;
		acpit->month = tm.tm_mon + 1;
		acpit->day = tm.tm_mday;
		acpit->minute = tm.tm_min;
		acpit->second = tm.tm_sec;
		acpit->milliseconds = 0;
	} else {
		printk(KERN_ERR
		       "%s: Invalid RTC value: write of %lx to ACPI TAD failed\n",
		       __FUNCTION__, nowtime);
		return -1;
	}

	ret = acpi_set_time(acpit);
	if (ret) {
		pr_err("acpi: can't write time!\n");
		return -1;
	}

	return 0;
}
#endif

static __init acpi_status
acpitad_parse_device(acpi_handle handle, u32 Level, void *context, void **retval)
{
	acpi_status status;

	/* evaluate _GCP */
	status = acpi_evaluate_integer(handle, "_GCP", NULL, &cap);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _GCP"));
		return -ENODEV;
	}

	*(acpi_handle *) context = handle;

	return status;
}

int __init acpi_tad_init(void)
{
	int result = 0;

	result = acpi_bus_register_driver(&acpi_time_alarm_driver);
	if (result < 0)
		return -ENODEV;

	return result;
}

int __init acpi_tad_parse(void)
{
	return acpi_get_devices("ACPI000E", acpitad_parse_device,
				&acpi_tad_handle, NULL);
}

static int acpi_read_timezone(s16 *timezone)
{
	struct acpi_time *acpit;
	int ret;

	acpit = kzalloc(sizeof(struct acpi_time), GFP_KERNEL);
	if (!acpit)
		return -ENOMEM;

	ret = acpi_read_time(acpit);
	if (ret) {
		/* should never happen */
		pr_err("acpi: can't read timezone.\n");
		ret = -EINVAL;
		goto error_read;
	}

	*timezone = (s16)le16_to_cpu(acpit->timezone);

error_read:
	kfree(acpit);

	return ret;
}

void __init acpi_tad_warp_clock(void)
{
	s16 timezone = 2047;

	if (!acpi_read_timezone(&timezone)) {
		/* TimeZone value, 2047 or 0 means RTC time is UTC */
		if (timezone != 0 && timezone != 2047) {
			struct timespec adjust;

			persistent_clock_is_local = 1;
			adjust.tv_sec = timezone * 60;
			adjust.tv_nsec = 0;
			timekeeping_inject_offset(&adjust);
			pr_info("acpi: RTC timezone is %d mins behind of UTC.\n", timezone);
			pr_info("acpi: Adjusted system time to UTC.\n");
		}
	}
}
