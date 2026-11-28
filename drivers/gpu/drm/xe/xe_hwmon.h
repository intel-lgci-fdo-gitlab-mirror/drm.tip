/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_HWMON_H_
#define _XE_HWMON_H_

#include <linux/types.h>

struct xe_device;

#if IS_REACHABLE(CONFIG_HWMON)
int xe_hwmon_register(struct xe_device *xe);
void xe_hwmon_unregister(struct xe_device *xe);
void xe_hwmon_suspend(struct xe_device *xe, bool runtime);
void xe_hwmon_resume(struct xe_device *xe, bool runtime);
void xe_hwmon_fan_update_post_lb(struct xe_device *xe);
#else
static inline int xe_hwmon_register(struct xe_device *xe) { return 0; };
static inline void xe_hwmon_unregister(struct xe_device *xe) { }
static inline void xe_hwmon_suspend(struct xe_device *xe, bool runtime) {}
static inline void xe_hwmon_resume(struct xe_device *xe, bool runtime) {}
static inline void xe_hwmon_fan_update_post_lb(struct xe_device *xe) {}
#endif

#endif /* _XE_HWMON_H_ */
