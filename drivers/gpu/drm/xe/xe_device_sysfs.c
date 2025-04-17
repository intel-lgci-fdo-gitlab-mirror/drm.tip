// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/kobject.h>
#include <linux/pci.h>
#include <linux/sysfs.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_device_sysfs.h"
#include "xe_mmio.h"
#include "xe_pcode_api.h"
#include "xe_pcode.h"
#include "xe_pm.h"

/**
 * DOC: Xe device sysfs
 * Xe driver requires exposing certain tunable knobs controlled by user space for
 * each graphics device. Considering this, we need to add sysfs attributes at device
 * level granularity.
 * These sysfs attributes will be available under pci device kobj directory.
 *
 * vram_d3cold_threshold - Report/change vram used threshold(in MB) below
 * which vram save/restore is permissible during runtime D3cold entry/exit.
 */

static ssize_t
vram_d3cold_threshold_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xe_device *xe = pdev_to_xe_device(pdev);
	int ret;

	xe_pm_runtime_get(xe);
	ret = sysfs_emit(buf, "%d\n", xe->d3cold.vram_threshold);
	xe_pm_runtime_put(xe);

	return ret;
}

static ssize_t
vram_d3cold_threshold_store(struct device *dev, struct device_attribute *attr,
			    const char *buff, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xe_device *xe = pdev_to_xe_device(pdev);
	u32 vram_d3cold_threshold;
	int ret;

	ret = kstrtou32(buff, 0, &vram_d3cold_threshold);
	if (ret)
		return ret;

	drm_dbg(&xe->drm, "vram_d3cold_threshold: %u\n", vram_d3cold_threshold);

	xe_pm_runtime_get(xe);
	ret = xe_pm_set_vram_threshold(xe, vram_d3cold_threshold);
	xe_pm_runtime_put(xe);

	return ret ?: count;
}

static DEVICE_ATTR_RW(vram_d3cold_threshold);

static void xe_pm_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;

	sysfs_remove_file(&xe->drm.dev->kobj, &dev_attr_vram_d3cold_threshold.attr);
}

int xe_pm_sysfs_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;
	int ret;

	ret = sysfs_create_file(&dev->kobj, &dev_attr_vram_d3cold_threshold.attr);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, xe_pm_sysfs_fini, xe);
}

/**
 * DOC: PCIe Gen5 Update Limitations
 *
 * Default link speed of discrete GPUs is determined by configuration
 * parameters stored in their flash memory, which are subject to override
 * through user initiated firmware updates. It has been observed that devices
 * configured with PCIe Gen5 as their default speed can come across link
 * quality issues due to host or motherboard limitations and may have to
 * auto-downspeed to PCIe Gen4 when faced with unstable link at Gen5, which
 * makes firmware updates rather risky on such setups. It is required to
 * ensure that the device is capable of auto-downspeeding to PCIe Gen4 link
 * before pushing the image with PCIe Gen5 as default configuration. This
 * can be done by reading ``pcie_gen4_downspeed_capable`` sysfs entry, which
 * will denote PCIe Gen4 downspeed capability of the device with boolean output
 * value of ``0`` or ``1``, meaning `incapable` or `capable` respectively.
 *
 * .. code-block:: shell
 *
 *    $ cat /sys/bus/pci/devices/<bdf>/pcie_gen4_downspeed_capable
 *
 * Pushing PCIe Gen5 update on a downspeed incapable device and facing link
 * instability due to host or motherboard limitations can result in driver
 * failing to bind to the device, making further firmware updates impossible
 * with RMA being the only last resort.
 *
 * PCIe Gen4 downspeed status of downspeed capable devices is available through
 * ``pcie_gen4_downspeed_status`` sysfs entry with boolean output value of
 * ``0`` or ``1``, where ``0`` means no auto-downspeeding was required during
 * link training (which is the optimal scenario) and ``1`` means the device
 * has auto-downsped to PCIe Gen4 due to unstable Gen5 link.
 *
 * .. code-block:: shell
 *
 *    $ cat /sys/bus/pci/devices/<bdf>/pcie_gen4_downspeed_status
 */

static ssize_t
pcie_gen4_downspeed_capable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xe_device *xe = pdev_to_xe_device(pdev);
	u32 cap, val;

	xe_pm_runtime_get(xe);
	val = xe_mmio_read32(xe_root_tile_mmio(xe), BMG_PCIE4_CAP);
	xe_pm_runtime_put(xe);

	cap = REG_FIELD_GET(PCIE4_DOWNSPEED, val);
	return sysfs_emit(buf, "%u\n", cap == DOWNSPEED_CAPABLE ? true : false);
}
static DEVICE_ATTR_ADMIN_RO(pcie_gen4_downspeed_capable);

static ssize_t
pcie_gen4_downspeed_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xe_device *xe = pdev_to_xe_device(pdev);
	u32 val;
	int ret;

	xe_pm_runtime_get(xe);
	ret = xe_pcode_read(xe_device_get_root_tile(xe),
			    PCODE_MBOX(DGFX_PCODE_STATUS, DGFX_GET_INIT_STATUS, 0),
			    &val, NULL);
	xe_pm_runtime_put(xe);

	return ret ?: sysfs_emit(buf, "%u\n", REG_FIELD_GET(DGFX_PCIE4_DOWNSPEED_STATUS, val));
}
static DEVICE_ATTR_ADMIN_RO(pcie_gen4_downspeed_status);

static const struct attribute *pcie_gen4_downspeed_attrs[] = {
	&dev_attr_pcie_gen4_downspeed_capable.attr,
	&dev_attr_pcie_gen4_downspeed_status.attr,
	NULL,
};

static void xe_device_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;

	if (xe->info.platform == XE_BATTLEMAGE)
		sysfs_remove_files(&xe->drm.dev->kobj, pcie_gen4_downspeed_attrs);
}

int xe_device_sysfs_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;
	int ret;

	if (xe->info.platform == XE_BATTLEMAGE) {
		ret = sysfs_create_files(&dev->kobj, pcie_gen4_downspeed_attrs);
		if (ret)
			return ret;
	}

	return devm_add_action_or_reset(dev, xe_device_sysfs_fini, xe);
}
