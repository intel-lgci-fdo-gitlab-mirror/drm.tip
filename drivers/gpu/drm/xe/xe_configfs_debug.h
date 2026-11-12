/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */
#ifndef _XE_CONFIGFS_DEBUG_H_
#define _XE_CONFIGFS_DEBUG_H_

#if IS_ENABLED(CONFIG_DRM_XE_DEBUG) && IS_ENABLED(CONFIG_CONFIGFS_FS)
struct config_item_type;
extern const struct config_item_type xe_configfs_debug_type;
#endif

#endif /* _XE_CONFIGFS_DEBUG_H_ */
