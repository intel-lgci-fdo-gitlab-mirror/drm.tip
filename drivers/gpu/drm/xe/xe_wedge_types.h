/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_WEDGE_TYPES_H_
#define _XE_WEDGE_TYPES_H_

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock_types.h>
#include <linux/wait.h>
#include <linux/workqueue_types.h>

/**
 * enum xe_wedged_mode - Possible wedge modes
 * @XE_WEDGED_MODE_NEVER: Device will never be declared wedged.
 * @XE_WEDGED_MODE_UPON_CRITICAL_ERROR: Device will be declared wedged only
 *	when critical error occurs like GT reset failure or firmware failure.
 *	This is the default mode.
 * @XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET: Device will be declared wedged on
 *	any hang. In this mode, engine resets are disabled to avoid automatic
 *	recovery attempts. This mode is primarily intended for debugging hangs.
 * @XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE: Stop submission
 *	for inspection and wait for a debugfs skip or continue response.
 */
enum xe_wedged_mode {
	XE_WEDGED_MODE_NEVER = 0,
	XE_WEDGED_MODE_UPON_CRITICAL_ERROR = 1,
	XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET = 2,
	XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE = 3,
};

/* Skip releases inspection without recovering the fault or restarting GTs. */
#define XE_WEDGE_REPLY_CONTINUE 0xdeadbbbb
#define XE_WEDGE_REPLY_SKIP 0xdeadf001

static inline bool xe_wedge_is_hanging_on_any(enum xe_wedged_mode mode)
{
	return mode == XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET ||
		mode == XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE;
}

/**
 * struct xe_wedge - Struct to control Wedged States and mode
 */
struct xe_wedge {
	/** @flag: Xe device faced a critical error and is now blocked. */
	atomic_t flag;
	/** @mode: Mode controlled by kernel parameter and debugfs */
	enum xe_wedged_mode mode;
	/** @method: Recovery method to be sent in the drm device wedged uevent */
	unsigned long method;
	/** @inconsistent_reset: Inconsistent reset policy state between GTs */
	bool inconsistent_reset;
	/** @work: Worker to defer non-atomic calls */
	struct work_struct work;
	/** @mode_mutex: Serializes mode changes and inspection teardown. */
	struct mutex mode_mutex;
	/** @mode_lock: Protects mode updates and inspection activation/replies. */
	spinlock_t mode_lock;
	/** @declare_pending: An inspection request owns the ioctl gate. */
	atomic_t declare_pending;
	/** @pause_work: Sleepable mode-3 stop and inspection worker. */
	struct work_struct pause_work;
	/** @pause_workqueue: Isolated queue for the indefinite inspection wait. */
	struct workqueue_struct *pause_workqueue;
	/** @pause_pm_ref: Queued worker's reference, protected by mode_lock. */
	bool pause_pm_ref;
	/** @pause_failed: Terminal mode-3 latch after runtime-resume failure. */
	bool pause_failed;
	/**
	 * @pause_stopped: Inspection stopped the GTs at least once.
	 * Remains set after SKIP or CONTINUE; neither restarts the GTs.
	 */
	bool pause_stopped;
	/** @pause_waitq: Wait for a reply or teardown. */
	wait_queue_head_t pause_waitq;
	/** @wait_active: Inspection is accepting replies under mode_lock. */
	bool wait_active;
	/** @shutting_down: Teardown blocks mode changes and new mode-3 requests. */
	bool shutting_down;
	/** @reply: Accepted debugfs response, protected by mode_lock. */
	u32 reply;
};

#endif
