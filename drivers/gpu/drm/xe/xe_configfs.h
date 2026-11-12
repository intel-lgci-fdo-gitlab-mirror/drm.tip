/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */
#ifndef _XE_CONFIGFS_H_
#define _XE_CONFIGFS_H_

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/types.h>

#include "xe_defaults.h"
#include "xe_hw_engine_types.h"
#include "xe_module.h"

struct pci_dev;

/**
 * DOC: survivability_mode configfs bits
 *
 * The survivability_mode attribute is a bitmap so that additional
 * survivability behaviors can be requested independently in the future.
 * %XE_CONFIGFS_SURVIVABILITY_ENABLE is currently the only defined bit, so
 * the attribute still only accepts 0 and 1.
 */
#define XE_CONFIGFS_SURVIVABILITY_ENABLE	BIT(0)
#define XE_CONFIGFS_SURVIVABILITY_MASK		XE_CONFIGFS_SURVIVABILITY_ENABLE

int xe_configfs_init(void);
void xe_configfs_exit(void);
void xe_configfs_check_device(struct pci_dev *pdev);
u8 xe_configfs_get_survivability_mode(struct pci_dev *pdev);
bool xe_configfs_primary_gt_allowed(struct pci_dev *pdev);
bool xe_configfs_media_gt_allowed(struct pci_dev *pdev);
u64 xe_configfs_get_engines_allowed(struct pci_dev *pdev);
bool xe_configfs_get_psmi_enabled(struct pci_dev *pdev);
bool xe_configfs_get_enable_multi_queue(struct pci_dev *pdev);
u32 xe_configfs_get_migrate_ulls_period_ms(struct pci_dev *pdev);
bool xe_configfs_get_enable_vram_page_offline(struct pci_dev *pdev);
ssize_t xe_configfs_get_ctx_restore_mid_bb(struct pci_dev *pdev,
					   enum xe_engine_class class,
					   u32 *cs, size_t max_len);
ssize_t xe_configfs_get_ctx_restore_post_bb(struct pci_dev *pdev,
					    enum xe_engine_class class,
					    u32 *cs, size_t max_len);
#ifdef CONFIG_PCI_IOV
unsigned int xe_configfs_get_max_vfs(struct pci_dev *pdev);
bool xe_configfs_admin_only_pf(struct pci_dev *pdev);
#endif

#endif
