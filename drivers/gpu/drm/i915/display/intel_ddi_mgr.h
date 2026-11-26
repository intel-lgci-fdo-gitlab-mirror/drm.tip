/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DDI_MGR_H__
#define __INTEL_DDI_MGR_H__

#include <linux/types.h>

enum port;

struct intel_display;

void intel_ddi_mgr_init(struct intel_display *display);

bool intel_ddi_mgr_is_pool_ddi(struct intel_display *display, enum port ddi);
bool intel_ddi_mgr_pool_supports_hdmi(struct intel_display *display);

#endif
