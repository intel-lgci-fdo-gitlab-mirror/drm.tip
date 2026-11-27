/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_PIPELOCK_H__
#define __INTEL_PIPELOCK_H__

#include <linux/types.h>

struct intel_connector;

bool intel_pipelock_connector_possible(const struct intel_connector *connector);

#endif /* __INTEL_PIPELOCK_H__ */
