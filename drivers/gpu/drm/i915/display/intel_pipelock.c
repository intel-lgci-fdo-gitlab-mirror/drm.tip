// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_pipelock.h"

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

bool intel_pipelock_connector_possible(const struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	return HAS_PIPELOCK(display) && connector->base.has_tile &&
		connector->base.connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
		!connector->mst.dp;
}
