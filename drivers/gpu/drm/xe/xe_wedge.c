// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/workqueue.h>

#include "xe_defaults.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_guc_ads.h"
#include "xe_log.h"
#include "xe_module.h"
#include "xe_pm.h"
#include "xe_printk.h"
#include "xe_sriov.h"
#include "xe_tlb_inval.h"
#include "xe_wedge.h"

/**
 * DOC: Xe Device Wedging
 *
 * Xe driver uses drm device wedged uevent as documented in Documentation/gpu/drm-uapi.rst.
 * When device is in wedged state, every IOCTL will be blocked and GT cannot
 * be used. The conditions under which the driver declares the device wedged
 * depend on the wedge mode configuration (see &enum xe_wedged_mode). The
 * default recovery method for a wedged state is rebind/bus-reset.
 *
 * Another recovery method is vendor-specific. Below are the cases that send
 * ``WEDGED=vendor-specific`` recovery method in drm device wedged uevent.
 *
 * Case: Firmware Flash
 * --------------------
 *
 * Identification Hint
 * +++++++++++++++++++
 *
 * ``WEDGED=vendor-specific`` drm device wedged uevent with
 * :ref:`Runtime Survivability mode <xe-survivability-mode>` is used to notify
 * admin/userspace consumer about the need for a firmware flash.
 *
 * Recovery Procedure
 * ++++++++++++++++++
 *
 * Once ``WEDGED=vendor-specific`` drm device wedged uevent is received, follow
 * the below steps
 *
 * - Check Runtime Survivability mode sysfs.
 *   If enabled, firmware flash is required to recover the device.
 *
 *   /sys/bus/pci/devices/<device>/survivability_mode
 *
 * - Admin/userspace consumer can use firmware flashing tools like fwupd to flash
 *   firmware and restore device to normal operation.
 */

static void wedge_work(struct work_struct *work)
{
	struct xe_device *xe = container_of(work, struct xe_device, wedged.work);

	/* Notify userspace of wedged device */
	drm_dev_wedged_event(&xe->drm, xe->wedged.method, NULL);
}

#define WEDGED_URL	"https://docs.kernel.org/gpu/drm-uapi.html#device-wedging"
#define XE_BUG_URL	"https://gitlab.freedesktop.org/drm/xe/kernel/issues/new"

/**
 * wedge_declare_now() - Declare device wedged
 * @xe: xe device instance
 * @method: method(s) to be used for recovery
 * @gts_already_wedged: Inspection has already stopped GT submission
 *
 * This is the final state that can only be cleared with the recovery method
 * specified in the drm wedged uevent.
 *
 * In this state every IOCTL will be blocked so the GT cannot be used.
 * In general it will be called upon any critical error such as gt reset
 * failure or guc loading failure. Userspace will be notified of this state
 * through device wedged uevent.
 *
 * If xe.wedged module parameter is set to 2, this function will be called
 * on every single execution timeout (a.k.a. GPU hang) right after devcoredump
 * snapshot capture. In this mode, GT reset won't be attempted so the state of
 * the issue is preserved for further debugging.
 *
 * Since the wedge handling is done as part of worker thread, this is safe for
 * atomic callers.
 */
static void wedge_declare_now(struct xe_device *xe, unsigned long method,
			      bool gts_already_wedged)
{
	struct xe_gt *gt;
	u8 id;

	if (xe->wedged.mode == XE_WEDGED_MODE_NEVER) {
		xe_dbg(xe, "Wedged mode is forcibly disabled\n");
		return;
	}

	if (atomic_xchg(&xe->wedged.flag, 1))
		return;

	xe->needs_flr_on_fini = true;
	xe_pm_runtime_get_noresume(xe);

	xe_log_err_fatal(xe, WEDGED, -EIO, "Device declared wedged!\n");
	xe_err_once(xe, "IOCTLs and executions are now blocked!\n"
			"For recovery procedure, refer to %s\n"
			"Please file a _new_ bug report at %s\n",
			WEDGED_URL, XE_BUG_URL);

	if (!gts_already_wedged) {
		for_each_initialized_gt(gt, xe, id)
			xe_gt_wedge_declare(gt);
	} else {
		for_each_initialized_gt(gt, xe, id)
			xe_tlb_inval_reset(&gt->tlb_inval);
	}

	/*
	 * XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET is intended for debugging
	 * hangs, so wedge the device with 'none' recovery method and have
	 * it available to the user for debugging.
	 */
	if (xe_wedge_is_hanging_on_any(READ_ONCE(xe->wedged.mode)))
		xe->wedged.method = DRM_WEDGE_RECOVERY_NONE;
	else
		xe->wedged.method = method;

	schedule_work(&xe->wedged.work);
}

static bool wedge_reply_ready(struct xe_device *xe)
{
	u32 reply = READ_ONCE(xe->wedged.reply);

	return reply == XE_WEDGE_REPLY_SKIP || reply == XE_WEDGE_REPLY_CONTINUE;
}

int xe_wedge_set_reply(struct xe_device *xe, u32 reply)
{
	unsigned long flags;
	int ret = 0;

	if (reply != XE_WEDGE_REPLY_SKIP && reply != XE_WEDGE_REPLY_CONTINUE)
		return -EINVAL;

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	if (READ_ONCE(xe->wedged.mode) !=
	    XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE ||
	    !atomic_read(&xe->wedged.declare_pending) ||
	    !xe->wedged.wait_active || READ_ONCE(xe->wedged.shutting_down))
		ret = -EINVAL;
	else if (READ_ONCE(xe->wedged.reply))
		ret = -EALREADY;
	else
		WRITE_ONCE(xe->wedged.reply, reply);
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);

	if (!ret)
		wake_up_all(&xe->wedged.pause_waitq);
	return ret;
}

static void wedge_pause_work(struct work_struct *work)
{
	struct xe_device *xe = container_of(work, struct xe_device, wedged.pause_work);
	struct xe_gt *gt;
	unsigned long flags;
	bool pm_ref;
	u32 reply;
	u8 id;

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	pm_ref = xe->wedged.pause_pm_ref;
	xe->wedged.pause_pm_ref = false;
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);

	if (READ_ONCE(xe->wedged.shutting_down)) {
		if (pm_ref)
			goto put_runtime_ref;
		goto clear_pending;
	}

	if (!pm_ref) {
		xe_warn(xe, "Mode-3 wedge could not retain active runtime PM; resuming for inspection, hardware state may have changed\n");
		if (!xe_pm_runtime_resume_and_get(xe)) {
			xe_err(xe, "Failed to resume device for mode-3 inspection; aborting pause\n");
			spin_lock_irqsave(&xe->wedged.mode_lock, flags);
			WRITE_ONCE(xe->wedged.pause_failed, true);
			atomic_set(&xe->wedged.declare_pending, 0);
			spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
			return;
		}
	}

	if (READ_ONCE(xe->wedged.shutting_down))
		goto put_runtime_ref;

	for_each_initialized_gt(gt, xe, id)
		xe_gt_wedge_pause(gt);
	WRITE_ONCE(xe->wedged.pause_stopped, true);

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	WRITE_ONCE(xe->wedged.reply, 0);
	xe->wedged.wait_active = true;
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);

	xe_err(xe, "GT submission stopped for mode-3 hardware debugging.\n"
	       "New DRM ioctls are blocked while waiting for 'wedged_pause_resume'.\n"
	       "Pending TLB invalidations are preserved until continue or another reset.\n"
	       "Skip: 0x%08x; continue into permanent wedge: 0x%08x.\n"
	       "Skip does not restart GTs or recover the fault.\n",
	       XE_WEDGE_REPLY_SKIP, XE_WEDGE_REPLY_CONTINUE);

	wait_event_idle(xe->wedged.pause_waitq,
			wedge_reply_ready(xe) || READ_ONCE(xe->wedged.shutting_down));

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	xe->wedged.wait_active = false;
	reply = READ_ONCE(xe->wedged.reply);
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);

	if (!READ_ONCE(xe->wedged.shutting_down) && reply == XE_WEDGE_REPLY_CONTINUE)
		wedge_declare_now(xe, DRM_WEDGE_RECOVERY_NONE, true);
	else
		xe_info(xe, "Inspection released without permanent wedge; GTs remain stopped\n");
put_runtime_ref:
	xe_pm_runtime_put(xe);
clear_pending:
	atomic_set(&xe->wedged.declare_pending, 0);
}

static void wedge_request(struct xe_device *xe, unsigned long method, bool mode3_only)
{
	unsigned long flags;
	enum xe_wedged_mode mode;
	bool pm_ref;

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	mode = READ_ONCE(xe->wedged.mode);
	if (mode3_only && mode != XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE) {
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		return;
	}

	if (mode != XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE) {
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		wedge_declare_now(xe, method, false);
		return;
	}

	if (READ_ONCE(xe->wedged.shutting_down) ||
	    atomic_read(&xe->wedged.declare_pending) ||
	    READ_ONCE(xe->wedged.pause_failed) ||
	    READ_ONCE(xe->wedged.pause_stopped) || xe_wedged(xe)) {
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		return;
	}

	atomic_set(&xe->wedged.declare_pending, 1);
	pm_ref = xe_pm_runtime_get_if_active(xe);
	if (WARN_ON_ONCE(!queue_work(xe->wedged.pause_workqueue, &xe->wedged.pause_work))) {
		xe_err(xe, "Mode-3 inspection work already queued\n");
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		if (pm_ref)
			xe_pm_runtime_put(xe);
		return;
	}
	xe->wedged.pause_pm_ref = pm_ref;
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
}

/**
 * xe_wedge_declare() - Request device wedging
 * @xe: Xe device
 * @method: Recovery methods for a permanent wedge
 *
 * Mode 3 defers all requests, including critical errors, to inspection.
 * Returning does not imply GT stopping is complete. Permanent wedging and
 * its userspace notification occur only on continue. Teardown suppresses
 * new inspection requests. Other modes retain normal wedge handling.
 */
void xe_wedge_declare(struct xe_device *xe, unsigned long method)
{
	wedge_request(xe, method, false);
}

/**
 * xe_wedge_declare_mode3() - Request inspection only in mode 3
 * @xe: Xe device
 *
 * Safe for atomic callers. Returning does not imply the GTs are stopped.
 */
void xe_wedge_declare_mode3(struct xe_device *xe)
{
	wedge_request(xe, DRM_WEDGE_RECOVERY_NONE, true);
}

/**
 * xe_wedge_prepare_fini() - Release and join mode-3 inspection before teardown
 * @xe: Xe device
 */
void xe_wedge_prepare_fini(struct xe_device *xe)
{
	unsigned long flags;

	mutex_lock(&xe->wedged.mode_mutex);
	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	WRITE_ONCE(xe->wedged.shutting_down, true);
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
	if (xe->wedged.pause_workqueue) {
		wake_up_all(&xe->wedged.pause_waitq);
		flush_work(&xe->wedged.pause_work);
	}
	mutex_unlock(&xe->wedged.mode_mutex);
}

static const char *wedge_mode_to_string(enum xe_wedged_mode mode)
{
	switch (mode) {
	case XE_WEDGED_MODE_NEVER:
		return "never";
	case XE_WEDGED_MODE_UPON_CRITICAL_ERROR:
		return "upon-critical-error";
	case XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET:
		return "upon-any-hang-no-reset";
	case XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE:
		return "upon-any-hang-no-reset-wait-for-release";
	default:
		return "<invalid>";
	}
}

static int wedge_validate_mode(struct xe_device *xe, unsigned int mode)
{
	if (mode > XE_WEDGED_MODE_UPON_ANY_HANG_NO_RESET_WAIT_FOR_RELEASE) {
		xe_dbg(xe, "wedged_mode: invalid value (%u)\n", mode);
		return -EINVAL;
	} else if (xe_wedge_is_hanging_on_any(mode) && (IS_SRIOV_VF(xe) ||
		   (IS_SRIOV_PF(xe) && !IS_ENABLED(CONFIG_DRM_XE_DEBUG)))) {
		xe_dbg(xe, "wedged_mode: (%u) %s mode is not supported for %s\n",
		       mode, wedge_mode_to_string(mode),
		       xe_sriov_mode_to_string(xe_device_sriov_mode(xe)));
		return -EPERM;
	}

	return 0;
}

static bool wedge_mode_needs_policy_update(struct xe_device *xe, enum xe_wedged_mode mode)
{
	if (xe->wedged.inconsistent_reset)
		return true;

	if (READ_ONCE(xe->wedged.mode) == mode)
		return false;

	if (xe_wedge_is_hanging_on_any(READ_ONCE(xe->wedged.mode)) ||
	    xe_wedge_is_hanging_on_any(mode))
		return true;

	return false;
}

static int __wedge_mode_set_reset_policy(struct xe_gt *gt, enum xe_wedged_mode mode)
{
	bool enable_engine_reset;
	int ret;

	enable_engine_reset = !xe_wedge_is_hanging_on_any(mode);
	ret = xe_guc_ads_scheduler_policy_toggle_reset(&gt->uc.guc.ads,
						       enable_engine_reset);
	if (ret)
		xe_gt_err(gt, "Failed to update GuC ADS scheduler policy (%pe)\n", ERR_PTR(ret));

	return ret;
}

static int wedge_mode_set_reset_policy(struct xe_device *xe, enum xe_wedged_mode mode)
{
	struct xe_gt *gt;
	int ret;
	u8 id;

	guard(xe_pm_runtime)(xe);
	for_each_gt(gt, xe, id) {
		ret = __wedge_mode_set_reset_policy(gt, mode);
		if (ret) {
			if (id > 0) {
				xe->wedged.inconsistent_reset = true;
				xe_err(xe, "Inconsistent reset policy state between GTs\n");
			}
			return ret;
		}
	}

	xe->wedged.inconsistent_reset = false;

	return 0;
}

/**
 * xe_wedge_set_mode() - Set wedge mode
 * @xe: xe device instance
 * @mode: wedge mode to be set
 *
 * Returns: 0 on success, negative error code otherwise.
 */
int xe_wedge_set_mode(struct xe_device *xe, enum xe_wedged_mode mode)
{
	unsigned long flags;
	enum xe_wedged_mode old_mode;
	bool update_policy;
	int ret;

	ret = wedge_validate_mode(xe, mode);
	if (ret)
		return ret;

	mutex_lock(&xe->wedged.mode_mutex);
	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	if (atomic_read(&xe->wedged.declare_pending) || READ_ONCE(xe->wedged.shutting_down)) {
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		ret = -EBUSY;
		goto out_unlock;
	}
	old_mode = READ_ONCE(xe->wedged.mode);
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);

	update_policy = wedge_mode_needs_policy_update(xe, mode);
	if (update_policy) {
		ret = wedge_mode_set_reset_policy(xe, mode);
		if (ret)
			goto out_unlock;
	}

	spin_lock_irqsave(&xe->wedged.mode_lock, flags);
	if (atomic_read(&xe->wedged.declare_pending)) {
		spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
		ret = update_policy ? wedge_mode_set_reset_policy(xe, old_mode) : 0;
		if (!ret)
			ret = -EBUSY;
		goto out_unlock;
	}
	WRITE_ONCE(xe->wedged.mode, mode);
	spin_unlock_irqrestore(&xe->wedged.mode_lock, flags);
	ret = 0;

out_unlock:
	mutex_unlock(&xe->wedged.mode_mutex);
	return ret;
}

static void xe_wedge_fini(struct drm_device *drm, void *arg)
{
	struct xe_device *xe = arg;

	xe_wedge_prepare_fini(xe);
	if (xe->wedged.pause_workqueue)
		destroy_workqueue(xe->wedged.pause_workqueue);
	disable_work_sync(&xe->wedged.work);

	if (atomic_read(&xe->wedged.flag))
		xe_pm_runtime_put(xe);
}

/**
 * xe_wedge_init() - Initialize wedge
 * @xe: xe device instance
 *
 * Returns: 0 on success, negative error code otherwise.
 */
int xe_wedge_init(struct xe_device *xe)
{
	xe->wedged.pause_stopped = false;
	INIT_WORK(&xe->wedged.work, wedge_work);
	INIT_WORK(&xe->wedged.pause_work, wedge_pause_work);
	xe->wedged.pause_workqueue = alloc_workqueue("xe_wedge_pause", WQ_UNBOUND, 1);
	if (!xe->wedged.pause_workqueue)
		return -ENOMEM;

	xe->wedged.mode = wedge_validate_mode(xe, xe_modparam.wedged_mode) ?
			  XE_DEFAULT_WEDGED_MODE : xe_modparam.wedged_mode;
	xe_dbg(xe, "wedged_mode: setting mode (%u) %s\n",
	       xe->wedged.mode, wedge_mode_to_string(xe->wedged.mode));

	return drmm_add_action_or_reset(&xe->drm, xe_wedge_fini, xe);
}
