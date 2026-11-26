// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_pipelock.h"

bool intel_pipelock_connector_possible(const struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	return HAS_PIPELOCK(display) && connector->base.has_tile &&
		connector->base.connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
		!connector->mst.dp;
}
