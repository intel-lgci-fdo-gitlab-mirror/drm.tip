// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Intel Corporation.
 */

#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/pci_ids.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "regs/xe_i2c_regs.h"

#include "xe_amc.h"
#include "xe_any.h"
#include "xe_device.h"
#include "xe_i2c.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_wedge.h"

/**
 * DOC: Add-In Management Controller (AMC)
 *
 * Handler for the SMBus Alerts from the AMC. All the alerts from AMC will cause
 * the device to be declared wedged.
 */

#define AMC_COMMAND		0x0f
#define AMC_GPU_I2C_ADDR	0x8f
#define AMC_VERSION_V1		0x01
#define AMC_DESTINATION_ID	12
#define AMC_SOURCE_ID		8
#define AMC_FLAGS		0xc8

#define AMC_MSG_TYPE		0x7e
#define AMC_GET_ALERT_REASON	0x01

enum xe_amc_alert {
	AMC_ALERT_UNKNOWN,
	AMC_ALERT_FW_DOWNLOAD,
	AMC_ALERT_THERMAL_TRIP,
	AMC_ALERT_OOB_REQUEST,
	AMC_ALERT_OOB_RESET,
	AMC_ALERT_CATERR,
};

static const char * const amc_alert[] = {
	[AMC_ALERT_UNKNOWN]		= "unknown",
	[AMC_ALERT_FW_DOWNLOAD]		= "Firmware Download",
	[AMC_ALERT_THERMAL_TRIP]	= "Thermal Trip",
	[AMC_ALERT_OOB_REQUEST]		= "OOB Request",
	[AMC_ALERT_OOB_RESET]		= "OOB Reset",
	[AMC_ALERT_CATERR]		= "Catastrophic",
};

struct xe_amc {
	struct xe_i2c *i2c;
	struct work_struct work;
	enum xe_amc_alert alert_reason;
	bool alert_seen;
};

struct amc_header {
	u8 command;
	u8 len;
	u8 address;
	u8 version;
	u8 destination;
	u8 source;
	u8 flags;
} __packed;

struct amc_message {
	u8 type;
	u16 vendor;
	u8 command;
} __packed;

struct amc_request {
	struct amc_header header;
	struct amc_message message;
	u32 reserved;
} __packed;

struct amc_response {
	struct amc_header header;
	struct amc_message message;
	u8 error;
	u8 value;
} __packed;

static const struct amc_request amc_get_alert_reason = {
	.header = {
		.command	= AMC_COMMAND,
		.len		= sizeof(struct amc_request) - 2,
		.address	= AMC_GPU_I2C_ADDR,
		.version	= AMC_VERSION_V1,
		.destination	= AMC_DESTINATION_ID,
		.source		= AMC_SOURCE_ID,
		.flags		= AMC_FLAGS,
	},
	.message = {
		.type		= AMC_MSG_TYPE,
		.vendor		= htons(PCI_VENDOR_ID_INTEL),
		.command	= AMC_GET_ALERT_REASON,
	},
};

/**
 * DOC: AMC Alert Reason
 *
 * Expose the most recent AMC alert reason through device sysfs.
 *
 * See Documentation/ABI/testing/sysfs-driver-intel-xe-amc for the ABI
 * specification.
 */

static enum xe_amc_alert amc_alert_reason(struct xe_amc *amc)
{
	return READ_ONCE(amc->alert_reason);
}

static ssize_t amc_alert_reason_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct xe_device *xe = pdev_to_xe_device(to_pci_dev(dev));
	struct xe_amc *amc = xe->i2c->amc;
	enum xe_amc_alert alert_reason;

	if (!READ_ONCE(amc->alert_seen))
		return sysfs_emit(buf, "none\n");

	alert_reason = amc_alert_reason(amc);

	return sysfs_emit(buf, "%s\n", amc_alert[alert_reason]);
}
static DEVICE_ATTR_RO(amc_alert_reason);

static void amc_remove_alert_sysfs(struct xe_i2c *i2c)
{
	struct device *dev = i2c->drm_dev;

	device_remove_file(dev, &dev_attr_amc_alert_reason);
}

static int amc_create_alert_sysfs(struct xe_i2c *i2c)
{
	struct device *dev = i2c->drm_dev;

	return device_create_file(dev, &dev_attr_amc_alert_reason);
}

static void xe_amc_work(struct work_struct *work)
{
	const struct amc_request *request = &amc_get_alert_reason;
	struct xe_amc *amc = from_work(amc, work, work);
	struct xe_device *xe = xe_any_to_xe(amc->i2c->drm_dev);
	u8 alert_reason = AMC_ALERT_UNKNOWN;
	struct amc_response response;
	struct i2c_client *client;
	const char *alert_str;
	unsigned long recovery = 0;
	int ret;

	xe_pm_runtime_get(xe);

	client = amc->i2c->client[XE_I2C_CLIENT_AMC];
	if (IS_ERR_OR_NULL(client))
		goto out_reassert_interrupt;

	ret = i2c_master_send(client, (u8 *)request, sizeof(*request));
	if (ret < 0) {
		dev_err(&client->dev, "failed to send request (%d)\n", ret);
		goto out_reassert_interrupt;
	}

	/* AMC needs 20ms to generate the response. */
	fsleep(20 * USEC_PER_MSEC);

	ret = i2c_master_recv(client, (u8 *)&response, sizeof(response));
	if (ret < 0) {
		dev_err(&client->dev, "failed to read response (%d)\n", ret);
		goto out_reassert_interrupt;
	}

	if (!response.header.len) {
		dev_err(&client->dev, "empty response from AMC\n");
		goto out_reassert_interrupt;
	}

	if (memcmp(&response.message, &request->message, sizeof(struct amc_message))) {
		dev_err(&client->dev, "response does not match the request\n");
		goto out_reassert_interrupt;
	}

	if (response.error) {
		dev_err(&client->dev, "AMC error 0x%02x\n", response.error);
		goto out_reassert_interrupt;
	}

	alert_reason = response.value;
	dev_dbg(&client->dev, "Alert reason: %u\n", (unsigned int)alert_reason);

out_reassert_interrupt:
	xe_mmio_rmw32(amc->i2c->mmio, I2C_CONFIG_CMD, PCI_COMMAND_INTX_DISABLE, 0);

	WRITE_ONCE(amc->alert_reason, alert_reason < ARRAY_SIZE(amc_alert) ?
		   alert_reason : AMC_ALERT_UNKNOWN);
	WRITE_ONCE(amc->alert_seen, true);

	switch (alert_reason) {
	case AMC_ALERT_FW_DOWNLOAD:
	case AMC_ALERT_OOB_RESET:
		alert_str = amc_alert[alert_reason];
		recovery = DRM_WEDGE_RECOVERY_REBIND | DRM_WEDGE_RECOVERY_BUS_RESET;
		break;
	case AMC_ALERT_CATERR:
	case AMC_ALERT_OOB_REQUEST:
	case AMC_ALERT_THERMAL_TRIP:
		alert_str = amc_alert[alert_reason];
		recovery = DRM_WEDGE_RECOVERY_COLD_RESET;
		break;
	default:
		alert_str = amc_alert[AMC_ALERT_UNKNOWN];
		break;
	}

	dev_warn(amc->i2c->drm_dev, "AMC Alert: %s (%u)\n", alert_str, (unsigned int)alert_reason);

	if (recovery)
		xe_wedge_declare(i2c_client_to_xe_device(client), recovery);

	xe_pm_runtime_put(xe);
}

void xe_amc_handle_alert(struct xe_i2c *i2c)
{
	queue_work(system_long_wq, &i2c->amc->work);
}

int xe_amc_init(struct xe_i2c *i2c)
{
	struct xe_amc *amc;
	int ret;

	amc = kzalloc_obj(*amc);
	if (!amc)
		return -ENOMEM;

	INIT_WORK(&amc->work, xe_amc_work);
	i2c->amc = amc;
	amc->i2c = i2c;

	ret = amc_create_alert_sysfs(i2c);
	if (ret) {
		kfree(i2c->amc);
		i2c->amc = NULL;
	}

	return ret;
}

void xe_amc_exit(struct xe_i2c *i2c)
{
	if (i2c->amc) {
		amc_remove_alert_sysfs(i2c);
		cancel_work_sync(&i2c->amc->work);
		kfree(i2c->amc);
		i2c->amc = NULL;
	}
}
