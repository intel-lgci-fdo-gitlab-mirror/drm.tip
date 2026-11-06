/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_WEDGE_H_
#define _XE_WEDGE_H_

#include <linux/atomic.h>
#include <linux/types.h>

#include "xe_device_types.h"
#include "xe_wedge_types.h"

static inline bool xe_wedged(struct xe_device *xe)
{
	return atomic_read(&xe->wedged.flag);
}

void xe_wedge_declare(struct xe_device *xe, unsigned long method);
void xe_wedge_declare_mode3(struct xe_device *xe);
void xe_wedge_prepare_fini(struct xe_device *xe);
int xe_wedge_set_reply(struct xe_device *xe, u32 reply);
int xe_wedge_init(struct xe_device *xe);
int xe_wedge_set_mode(struct xe_device *xe, enum xe_wedged_mode mode);

#endif
