/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */
#ifndef _XE_CONFIGFS_TYPES_H_
#define _XE_CONFIGFS_TYPES_H_

#include <linux/configfs.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "xe_hw_engine_types.h"
#include "xe_pci_types.h"
#include "xe_sriov_types.h"

/* Similar to struct xe_bb, but not tied to HW (yet) */
struct wa_bb {
	u32 *cs;
	u32 len; /* in dwords */
};

struct xe_config_group_device {
	struct config_group group;
	struct config_group sriov;

	/*
	 * Fields sorted by type (largest first) then name: struct arrays,
	 * u64, u32/unsigned int, u8, bool, sub-structs last. Within each
	 * type, entries are sorted alphabetically.
	 */
	struct xe_config_device {
		struct wa_bb ctx_restore_mid_bb[XE_ENGINE_CLASS_MAX];
		struct wa_bb ctx_restore_post_bb[XE_ENGINE_CLASS_MAX];
		u64 engines_allowed;
		u64 gt_types_allowed;
		u32 migrate_ulls_period_ms;
		u8 survivability_mode;
		bool enable_multi_queue;
		bool enable_psmi;
		bool enable_vram_page_offline;
		struct {
			unsigned int max_vfs;
			bool admin_only_pf;
		} sriov;
	} config;

	/* protects attributes */
	struct mutex lock;
	/* matching descriptor */
	const struct xe_device_desc *desc;
	/* tentative SR-IOV mode */
	enum xe_sriov_mode mode;
};

#endif /* _XE_CONFIGFS_TYPES_H_ */
