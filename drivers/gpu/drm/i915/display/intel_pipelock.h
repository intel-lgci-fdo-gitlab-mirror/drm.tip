/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __INTEL_PIPELOCK_H__
#define __INTEL_PIPELOCK_H__

#include <linux/types.h>

struct intel_atomic_state;
struct intel_connector;
struct intel_crtc_state;

bool intel_pipelock_connector_possible(const struct intel_connector *connector);
bool intel_pipelock_possible(const struct intel_crtc_state *crtc_state,
			     struct intel_connector *connector);
bool intel_pipelock_compatible(const struct intel_crtc_state *crtc_state1,
			       const struct intel_crtc_state *crtc_state2);
void intel_pipelock_compute_config(struct intel_atomic_state *state,
				   struct intel_crtc_state *crtc_state,
				   u16 transcoders);

#endif /* __INTEL_PIPELOCK_H__ */
