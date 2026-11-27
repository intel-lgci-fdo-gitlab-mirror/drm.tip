// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/math64.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dpll_mgr.h"
#include "intel_pipelock.h"
#include "intel_vrr.h"

/**
 * DOC: Transcoder Pipelock
 *
 * Pipelock is a local variant of Genlock. It locks the timing generators (TG)
 * of several transcoders in the same display engine together, so that they
 * run at the same frequency and phase. The driver uses it to keep the tiles
 * of a tiled display in sync, in place of transcoder port sync, which is
 * defeatured from Xe3 onwards. Pipelock is used on platforms where
 * HAS_PIPELOCK() is true, older platforms keep using port sync.
 *
 * One transcoder in a group is the primary, the others are secondaries. The
 * TG of every running transcoder sends out a frame sync event every frame,
 * at the start of its set context latency (SCL) window, or at the start of
 * the VRR guardband if there is no SCL window. A secondary is set up in
 * TRANS_DDI_FUNC_CTL2 as a local genlock secondary of its primary. Once it
 * sees a frame sync from the primary, it starts its own TG after a
 * programmable start delay. It either syncs just once, at modeset, or on
 * every frame, which also allows a variable refresh rate. The primary needs
 * no programming.
 *
 * The driver reuses the port sync primary/secondary state of the CRTCs for
 * the pipelock group. A group is only formed from DP SST tiles that can be
 * locked together:
 *
 * - The transcoders must have the same timings, link configuration, PLL
 *   state, bpp, SCL, VRR timings and SDPs. The VRR guardband must match too,
 *   so the guardband of each tile is raised to the largest one in its group.
 *
 * - Spread spectrum clocking (SSC), PSR, panel replay, content match refresh
 *   rate (CMRR) and DC balance can't be used on pipelocked transcoders. They
 *   are computed before the group is known, so they are kept off for every
 *   DP SST tile on platforms with pipelock.
 *
 * - DP to HDMI protocol converters retime the signal, so they can't be
 *   locked.
 *
 * - Syncing every frame needs the start delay to fit in a line. If it
 *   doesn't, the tiles only sync once, which requires a fixed refresh rate.
 *
 * A secondary raises no vblank interrupts until it has synced, so the
 * primary is enabled first and the secondaries after it. On disable the
 * secondaries still go first. A link training fallback on one tile is
 * applied to all the tiles of the group, so that they keep the same link
 * configuration.
 */

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
		crtc_state1->vrr.guardband == crtc_state2->vrr.guardband &&
		crtc_state1->infoframes.enable == crtc_state2->infoframes.enable &&
		intel_dpll_compare_hw_state(display, &crtc_state1->dpll_hw_state,
					    &crtc_state2->dpll_hw_state);
}

void intel_pipelock_compute_config(struct intel_crtc_state *crtc_state)
{
	if (crtc_state->sync_primary_transcoder == INVALID_TRANSCODER)
		return;

	crtc_state->pipelock.tg_start_delay = pipelock_tg_start_delay(crtc_state);
	crtc_state->pipelock.per_frame_sync = pipelock_per_frame_sync(crtc_state);
}
