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
#include <linux/module.h>
#include <linux/platform_device.h>
#include <acpi/acpi_drivers.h>

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

static struct acpi_device *acpi_tad_dev;
static unsigned long long cap;

int acpi_read_time(struct acpi_time *output)
{
	unsigned long flags;
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	struct acpi_time *acpit;
	acpi_status status;

	if (!acpi_tad_dev)
		return -ENODEV;

	if (!(cap & TAD_CAP_GETSETTIME))
		return -EINVAL;

	if (!output)
		return -EINVAL;

	spin_lock_irqsave(&rtc_lock, flags);
	status = acpi_evaluate_object(acpi_tad_dev->handle, "_GRT", NULL, &result);
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
		dev_err(&acpi_tad_dev->dev, ACPI_TIME_ALARM_NAME
			" Invalid _GRT data\n");
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

int acpi_set_time(struct acpi_time *acpit)
{
	unsigned long flags;
	struct acpi_object_list input;
	union acpi_object params[1];
	unsigned long long output;
	acpi_status status;

	if (!acpi_tad_dev)
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
	status = acpi_evaluate_integer(acpi_tad_dev->handle, "_SRT", &input, &output);
	spin_unlock_irqrestore(&rtc_lock, flags);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _SRT"));
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL(acpi_set_time);

int acpi_tad_get_capability(unsigned long *output)
{
	if (!acpi_tad_dev)
		return -ENODEV;

	*output = cap;

	return 0;
}
EXPORT_SYMBOL(acpi_tad_get_capability);

static int acpi_time_alarm_add(struct acpi_device *device)
{
	acpi_status status;

	if (!device)
		return -EINVAL;

	acpi_tad_dev = device;

	/* evaluate _GCP */
	status = acpi_evaluate_integer(device->handle, "_GCP", NULL, &cap);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating _GCP"));
		return -ENODEV;
	}

	if (!(cap & TAD_CAP_GETSETTIME))
		pr_warn(FW_INFO "Get/Set real time features not available.\n");

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

int __init acpi_tad_init(void)
{
	int result = 0;

	result = acpi_bus_register_driver(&acpi_time_alarm_driver);
	if (result < 0)
		return -ENODEV;

	return result;
}
