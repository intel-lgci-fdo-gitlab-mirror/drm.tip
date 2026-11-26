// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/math64.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_print.h>

#include "intel_display.h"
#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dpll_mgr.h"
#include "intel_pipelock.h"
#include "intel_vrr.h"

#define PIPELOCK_CLOCK_CROSSING_DELAY	8
#define MTP_TIME_SLOTS			64

bool intel_pipelock_connector_possible(const struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	return HAS_PIPELOCK(display) && connector->base.has_tile &&
		connector->base.connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
		!connector->mst.dp;
}

static int pipelock_tg_start_delay(const struct intel_crtc_state *crtc_state)
{
	const struct intel_link_m_n *m_n = &crtc_state->dp_m_n;
	int delay = PIPELOCK_CLOCK_CROSSING_DELAY;

	delay += DIV_ROUND_UP(m_n->link_n, m_n->link_m);

	if (intel_dp_is_uhbr(crtc_state))
		delay += MTP_TIME_SLOTS - m_n->tu;

	return delay;
}

static int pipelock_line_time(const struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	int symbol_clock = intel_dp_link_symbol_clock(crtc_state->port_clock);

	return div_u64(mul_u32_u32(adjusted_mode->crtc_htotal, symbol_clock),
		       adjusted_mode->crtc_clock);
}

static bool pipelock_per_frame_sync(const struct intel_crtc_state *crtc_state)
{
	return pipelock_tg_start_delay(crtc_state) < pipelock_line_time(crtc_state);
}

bool intel_pipelock_possible(const struct intel_crtc_state *crtc_state,
			     struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_dp *intel_dp = intel_attached_dp(connector);

	if (!intel_pipelock_connector_possible(connector))
		return false;

	if (drm_dp_is_branch(intel_dp->dpcd) && intel_dp_has_hdmi_sink(intel_dp)) {
		drm_dbg_kms(display->drm,
			    "[CONNECTOR:%d:%s] No pipelock behind an HDMI converter\n",
			    connector->base.base.id, connector->base.name);
		return false;
	}

	if (!pipelock_per_frame_sync(crtc_state) &&
	    (!intel_vrr_is_fixed_rr(crtc_state) || crtc_state->vrr.in_range)) {
		drm_dbg_kms(display->drm,
			    "[CONNECTOR:%d:%s] No pipelock, start delay too long for VRR\n",
			    connector->base.base.id, connector->base.name);
		return false;
	}

	return true;
}

bool intel_pipelock_compatible(const struct intel_crtc_state *crtc_state1,
			       const struct intel_crtc_state *crtc_state2)
{
	struct intel_display *display = to_intel_display(crtc_state1);

	return crtc_state1->pipe_bpp == crtc_state2->pipe_bpp &&
		crtc_state1->set_context_latency == crtc_state2->set_context_latency &&
		crtc_state1->vrr.enable == crtc_state2->vrr.enable &&
		crtc_state1->vrr.vmin == crtc_state2->vrr.vmin &&
		crtc_state1->vrr.vmax == crtc_state2->vrr.vmax &&
		crtc_state1->vrr.flipline == crtc_state2->vrr.flipline &&
		crtc_state1->infoframes.enable == crtc_state2->infoframes.enable &&
		intel_dpll_compare_hw_state(display, &crtc_state1->dpll_hw_state,
					    &crtc_state2->dpll_hw_state);
}

static int pipelock_group_guardband(struct intel_atomic_state *state, u16 transcoders)
{
	const struct intel_crtc_state *crtc_state;
	struct intel_crtc *crtc;
	int guardband = 0;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state) {
		if (transcoders & BIT(crtc_state->cpu_transcoder))
			guardband = max_t(int, guardband, crtc_state->vrr.guardband);
	}

	return guardband;
}

void intel_pipelock_compute_config(struct intel_atomic_state *state,
				   struct intel_crtc_state *crtc_state,
				   u16 transcoders)
{
	int guardband;

	if (!transcoders)
		return;

	guardband = pipelock_group_guardband(state, transcoders);
	if (guardband != crtc_state->vrr.guardband)
		intel_vrr_guardband_val(crtc_state, guardband);

	if (crtc_state->sync_primary_transcoder == INVALID_TRANSCODER)
		return;

	crtc_state->pipelock.tg_start_delay = pipelock_tg_start_delay(crtc_state);
	crtc_state->pipelock.per_frame_sync = pipelock_per_frame_sync(crtc_state);
}
