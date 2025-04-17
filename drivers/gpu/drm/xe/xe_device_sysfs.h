/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DEVICE_SYSFS_H_
#define _XE_DEVICE_SYSFS_H_

struct xe_device;

int xe_pm_sysfs_init(struct xe_device *xe);

#endif
