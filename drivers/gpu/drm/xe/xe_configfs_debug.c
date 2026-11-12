// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/configfs.h>
#include <linux/module.h>

#include "xe_configfs_debug.h"
#include "xe_configfs_types.h"

const struct config_item_type xe_configfs_debug_type = {
	.ct_owner	= THIS_MODULE,
};
