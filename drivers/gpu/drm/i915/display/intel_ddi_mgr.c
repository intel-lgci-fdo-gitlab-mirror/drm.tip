// SPDX-License-Identifier: MIT
/* Copyright © 2026 Intel Corporation */

#include <linux/iopoll.h>
#include <linux/seq_buf.h>
#include <linux/string.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "intel_bios.h"
#include "intel_ddi_mgr.h"
#include "intel_ddi_mgr_regs.h"
#include "intel_de.h"
#include "intel_display.h"
#include "intel_display_core.h"
#include "intel_display_regs.h"
#include "intel_display_rpm.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_dp.h"
#include "intel_gmbus.h"
#include "intel_hotplug_irq.h"
#include "intel_tc.h"

#define INTEL_DDI_MGR_NUM_PORTS 4

enum intel_ddi_route_mode {
	/* DP-alt/TBT-alt, with allocations managed by firmware. */
	INTEL_DDI_ROUTE_ALT,
	/* Legacy DP/HDMI, with allocations managed by the driver. */
	INTEL_DDI_ROUTE_LEGACY,
};

struct intel_ddi_route {
	/*
	 * Physical legacy PHY, or TC_PORT_NONE in ALT mode.
	 * TODO: Use a PHY enum instead of enum tc_port.
	 */
	enum tc_port phy;
	enum intel_ddi_route_mode mode;
};

struct intel_ddi_mgr {
	/* Only route entries with a bit set in pool_ddi_mask are used. */
	struct intel_ddi_route route[INTEL_DDI_MGR_NUM_PORTS];
	u8 ddc_pin[INTEL_DDI_MGR_NUM_PORTS];
	u8 pool_ddi_mask;
	u8 legacy_phy_mask;
	u16 ddc_pins;
	bool has_hdmi;
	bool enabled;
};

struct intel_ddi_hw_state {
	struct intel_ddi_route route[INTEL_DDI_MGR_NUM_PORTS];
	u32 inherited_resources;
	u32 current_resources;
	u8 pool_ddi_mask;
	u8 busy_ddi_mask;
	u8 assigned_phy_mask;
	u8 allocated_ddi_mask;
};

static enum port ddi_to_port(int ddi)
{
	return PORT_TC1 + ddi;
}

static enum tc_port ddi_to_tc(int ddi)
{
	return TC_PORT_1 + ddi;
}

static enum hpd_pin tc_to_hpd_pin(enum tc_port tc_port)
{
	return HPD_PORT_TC1 + tc_port - TC_PORT_1;
}

/* TODO: Fix the regular HDMI default selection on NVL+ and reuse it here. */
static u8 default_ddc_pin(struct intel_display *display, enum port port)
{
	if (port >= PORT_TC1 && port <= PORT_TC4)
		return intel_gmbus_tc_port_to_pin(intel_port_to_tc(display, port));

	return intel_port_to_phy(display, port) + 1;
}

static bool intel_ddi_mgr_is_enabled(const struct intel_ddi_mgr *mgr)
{
	return mgr && mgr->enabled;
}

bool intel_ddi_mgr_is_pool_ddi(struct intel_display *display, enum port ddi)
{
	return display->ddi_mgr && ddi >= PORT_TC1 && ddi <= PORT_TC4 &&
		display->ddi_mgr->pool_ddi_mask & BIT(ddi - PORT_TC1);
}

bool intel_ddi_mgr_pool_supports_hdmi(struct intel_display *display)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;

	return mgr && mgr->pool_ddi_mask && mgr->has_hdmi;
}

static const char *iom_lock_owner(u32 semaphore)
{
	switch (semaphore & IOM_REQUESTOR_ID_MASK) {
	case IOM_REQUESTOR_ID_IOM:
		return "IOM";
	case IOM_REQUESTOR_ID_TBT0:
		return "TBT0";
	case IOM_REQUESTOR_ID_TBT1:
		return "TBT1";
	case IOM_REQUESTOR_ID_DISPLAY_ENGINE:
		return "Display Engine";
	default:
		return "unknown";
	}
}

static bool iom_trylock(struct intel_display *display, u32 *semaphore)
{
	u32 request = IOM_DP_HW_SEMLOCK | IOM_REQUESTOR_ID_DISPLAY_ENGINE;
	u32 mask = IOM_DP_HW_SEMLOCK | IOM_REQUESTOR_ID_MASK;

	intel_de_rmw(display, IOM_DP_HW_RESOURCE_SEMAPHORE, mask, request);
	*semaphore = intel_de_read(display, IOM_DP_HW_RESOURCE_SEMAPHORE);

	return (*semaphore & mask) == request;
}

static int iom_lock(struct intel_display *display)
{
	u32 semaphore;
	bool acquired;
	int err;

	assert_display_rpm_held(display);

	intel_de_rmw(display, IOM_HIP_INDEX, IOM_HIP_INDEX_MASK,
		     IOM_HIP_INDEX_RESOURCE_MGMT);

	err = poll_timeout_us(acquired = iom_trylock(display, &semaphore),
			      acquired, 10, 100000, false);
	if (!err)
		return 0;

	drm_warn(display->drm,
		 "DDIMGR: IOM DDI lock timeout: owner %s, register %#x\n",
		 iom_lock_owner(semaphore), semaphore);

	return err;
}

static void iom_unlock(struct intel_display *display)
{
	intel_de_rmw(display, IOM_DP_HW_RESOURCE_SEMAPHORE,
		     IOM_DP_HW_SEMLOCK | IOM_REQUESTOR_ID_MASK, IOM_REQUESTOR_ID_DISPLAY_ENGINE);
	intel_de_posting_read(display, IOM_DP_HW_RESOURCE_SEMAPHORE);
}

static int resource_to_available_ddi_count(struct intel_display *display, u32 semaphore)
{
	u32 gray = REG_FIELD_GET(IOM_DDI_AVAILABLE_MASK, semaphore);

	switch (gray) {
	case 0x0:
		return 0;
	case 0x1:
		return 1;
	case 0x2:
		return 3;
	case 0x3:
		return 2;
	case 0x6:
		return IOM_DDI_REPORTED_MAX_FREE; /* At least four resources are available. */
	default:
		drm_WARN(display->drm, 1, "DDIMGR: Invalid DDI availability %#x (semaphore %#x)\n",
			 gray, semaphore);

		return -EINVAL;
	}
}

static u8 resource_to_pool_ddi_mask(u32 resources)
{
	u8 pool_ddi_mask = 0;
	int ddi;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++)
		if (IOM_DDI_CONSUMER_GET(resources, ddi) != IOM_DDI_CONSUMER_RESERVED)
			pool_ddi_mask |= BIT(ddi);

	return pool_ddi_mask;
}

static void verify_ddi_counts(struct intel_display *display, u32 resources)
{
	u32 semaphore;
	int available_ddis, free_ddis = 0;
	int ddi;

	semaphore = intel_de_read(display, IOM_DP_HW_RESOURCE_SEMAPHORE);
	available_ddis = resource_to_available_ddi_count(display, semaphore);

	/* The hardware count covers all eight slots, not just our four TC DDIs. */
	for (ddi = 0; ddi < IOM_DDI_NUM_SLOTS; ddi++)
		if (IOM_DDI_CONSUMER_GET(resources, ddi) == IOM_DDI_CONSUMER_FREE)
			free_ddis++;

	drm_WARN(display->drm, free_ddis > INTEL_DDI_MGR_NUM_PORTS,
		 "DDIMGR: Free DDI count %d exceeds supported count %d\n",
		 free_ddis, INTEL_DDI_MGR_NUM_PORTS);

	if (available_ddis < 0 || available_ddis == min(free_ddis, IOM_DDI_REPORTED_MAX_FREE))
		return;

	drm_dbg_kms(display->drm,
		    "DDIMGR: DDI count mismatch: available %d%s, free %d (semaphore %#x, table 0x%08x)\n",
		    available_ddis, available_ddis == IOM_DDI_REPORTED_MAX_FREE ? "+" : "",
		    free_ddis, semaphore, resources);
}

/* TC PHY consumer (legacy or DP-alt), rather than a TBT DP adapter. */
static bool consumer_is_dpalt_legacy(u32 consumer)
{
	return consumer & IOM_DDI_CONSUMER_DPALT_LEGACY &&
		REG_FIELD_GET(IOM_DDI_CONSUMER_PHY_MASK, consumer) < INTEL_DDI_MGR_NUM_PORTS;
}

/* Caller must first check consumer_is_dpalt_legacy(). */
static enum tc_port consumer_to_phy(u32 consumer)
{
	return TC_PORT_1 + REG_FIELD_GET(IOM_DDI_CONSUMER_PHY_MASK, consumer);
}

static void format_ddi_consumer(struct seq_buf *s, const struct intel_ddi_mgr *mgr,
				u32 consumer, const char *origin)
{
	if (consumer == IOM_DDI_CONSUMER_FREE) {
		seq_buf_puts(s, "free");

		return;
	}

	if (consumer == IOM_DDI_CONSUMER_RESERVED) {
		seq_buf_puts(s, "reserved");

		return;
	}

	if (consumer_is_dpalt_legacy(consumer)) {
		enum tc_port phy = consumer_to_phy(consumer);
		bool legacy = mgr->legacy_phy_mask & BIT(phy);

		if (!legacy)
			origin = NULL;

		seq_buf_printf(s, "PHY TC#%d (%s%s%s)", phy - TC_PORT_1 + 1,
			       legacy ? "legacy" : "dp-alt", origin ? ", " : "", origin ?: "");

		return;
	}

	if (!(consumer & IOM_DDI_CONSUMER_DPALT_LEGACY) &&
	    REG_FIELD_GET(IOM_DDI_CONSUMER_TBT_DPIN_MASK, consumer)) {
		seq_buf_printf(s, "TBT%u DPIN%u",
			       REG_FIELD_GET(IOM_DDI_CONSUMER_TBT_DOMAIN_MASK, consumer),
			       REG_FIELD_GET(IOM_DDI_CONSUMER_TBT_DPIN_MASK, consumer) - 1);

		return;
	}

	seq_buf_printf(s, "unknown(0x%x)", consumer);
}

static void print_ddi_allocations(struct drm_printer *p, const struct intel_ddi_mgr *mgr,
				  const struct intel_ddi_hw_state *hw_state, int err)
{
	bool printed = false;
	int ddi;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		u32 consumer = IOM_DDI_CONSUMER_GET(hw_state->current_resources, ddi);
		DECLARE_SEQ_BUF(name, 64);
		const char *origin = NULL;

		if (consumer == IOM_DDI_CONSUMER_FREE || consumer == IOM_DDI_CONSUMER_RESERVED)
			continue;

		if (consumer == IOM_DDI_CONSUMER_GET(hw_state->inherited_resources, ddi))
			origin = "inherited";
		else if (!err && hw_state->allocated_ddi_mask & BIT(ddi))
			origin = "allocated";

		format_ddi_consumer(&name, mgr, consumer, origin);
		drm_printf_indent(p, 1, "DDI TC#%d: %s\n", ddi + 1, seq_buf_str(&name));
		printed = true;
	}

	if (!printed)
		drm_printf_indent(p, 1, "none\n");
}

static void print_ddi_resources(struct intel_display *display,
				const struct intel_ddi_hw_state *hw_state, int err)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	struct drm_printer p = drm_dbg_printer(display->drm, DRM_UT_KMS, "DDIMGR:");
	unsigned long pool_ddi_mask = resource_to_pool_ddi_mask(hw_state->current_resources) << 1;
	unsigned long unassigned_phy_mask =
		(mgr->legacy_phy_mask & ~hw_state->assigned_phy_mask) << 1;

	drm_printf(&p, "Resources=0x%08x, Pool DDIs=DDI TC#{%*pbl}\n",
		   hw_state->current_resources, INTEL_DDI_MGR_NUM_PORTS + 1, &pool_ddi_mask);
	if (err)
		drm_printf(&p, "Configuration failed: %d\n", err);

	/* ALT allocations are only a snapshot of firmware assignments. */
	drm_printf(&p, "Assigned DDIs:\n");
	print_ddi_allocations(&p, mgr, hw_state, err);

	if (!err && unassigned_phy_mask)
		drm_printf(&p, "Unassigned legacy PHYs=PHY TC#{%*pbl}\n",
			   INTEL_DDI_MGR_NUM_PORTS + 1, &unassigned_phy_mask);
}

static int verify_ddi_pool(struct intel_display *display, u8 pool_ddi_mask)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	int ddi;

	if (mgr->pool_ddi_mask && pool_ddi_mask != mgr->pool_ddi_mask) {
		drm_dbg_kms(display->drm, "DDIMGR: DDI pool changed: expected %#x, read %#x\n",
			    mgr->pool_ddi_mask, pool_ddi_mask);

		return -EINVAL;
	}

	if (mgr->legacy_phy_mask & ~pool_ddi_mask) {
		drm_err(display->drm,
			"DDIMGR: Legacy PHYs missing from DDI pool: legacy=0x%02x pool=0x%02x\n",
			mgr->legacy_phy_mask, pool_ddi_mask);

		return -EINVAL;
	}

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		const struct intel_bios_encoder_data *vbt;
		bool alt_mode;

		if (!(pool_ddi_mask & BIT(ddi)))
			continue;

		vbt = intel_bios_encoder_data_lookup(display, ddi_to_port(ddi));
		if (!vbt) {
			drm_err(display->drm, "DDIMGR: Missing VBT data for pool DDI%d\n", ddi);

			return -EINVAL;
		}

		if (intel_bios_encoder_is_dedicated_external(vbt) ||
		    intel_bios_encoder_supports_edp(vbt) ||
		    intel_bios_encoder_supports_dsi(vbt) || intel_bios_encoder_is_lspcon(vbt)) {
			drm_err(display->drm,
				"DDIMGR: Unsupported pool DDI%d: dedicated=%d eDP=%d DSI=%d LSPCON=%d\n",
				ddi, intel_bios_encoder_is_dedicated_external(vbt),
				intel_bios_encoder_supports_edp(vbt),
				intel_bios_encoder_supports_dsi(vbt),
				intel_bios_encoder_is_lspcon(vbt));

			return -EINVAL;
		}

		alt_mode = intel_bios_encoder_supports_typec_usb(vbt) ||
			intel_bios_encoder_supports_tbt(vbt);
		if (alt_mode == !!(mgr->legacy_phy_mask & BIT(ddi))) {
			drm_err(display->drm,
				"DDIMGR: Invalid VBT modes for pool DDI%d: alt=%d dyn-legacy=%d\n",
				ddi, alt_mode, !!(mgr->legacy_phy_mask & BIT(ddi)));

			return -EINVAL;
		}
	}

	return 0;
}

static int verify_ddc_pins(struct intel_display *display, u8 pool_ddi_mask)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	enum port port;
	int err = 0;

	for (port = PORT_A; port < I915_MAX_PORTS; port++) {
		const struct intel_bios_encoder_data *vbt;
		int pin;

		if (port >= PORT_TC1 && port <= PORT_TC4 &&
		    pool_ddi_mask & BIT(port - PORT_TC1))
			continue;

		vbt = intel_bios_encoder_data_lookup(display, port);
		if (!vbt || (!intel_bios_encoder_supports_hdmi(vbt) &&
			     !intel_bios_encoder_supports_dvi(vbt)))
			continue;

		pin = intel_bios_hdmi_ddc_pin(vbt);
		/* Match intel_hdmi_default_ddc_pin() for non-pool encoders. */
		if (!pin)
			pin = intel_port_to_phy(display, port) + 1;

		if (pin >= GMBUS_NUM_PINS)
			continue;

		if (!(mgr->ddc_pins & BIT(pin)))
			continue;

		drm_err(display->drm, "DDIMGR: DDI pool DDC pin %d also used by port %c\n",
			pin, port_name(port));

		err = -EINVAL;
	}

	return err;
}

/* Record the DDI's mode and legacy PHY in software. */
static void record_ddi_route(struct intel_ddi_route *route, enum tc_port legacy_phy)
{
	route->phy = legacy_phy;
	route->mode = legacy_phy == TC_PORT_NONE ?
		INTEL_DDI_ROUTE_ALT : INTEL_DDI_ROUTE_LEGACY;
}

static void fallback_to_alt(struct intel_ddi_mgr *mgr)
{
	int ddi;

	mgr->enabled = false;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		if (!(mgr->pool_ddi_mask & BIT(ddi)))
			continue;

		record_ddi_route(&mgr->route[ddi], TC_PORT_NONE);
	}
}

static int readout_ddi_resource(struct intel_display *display, int ddi,
				struct intel_ddi_hw_state *hw_state)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	u32 consumer = IOM_DDI_CONSUMER_GET(hw_state->current_resources, ddi);
	enum tc_port phy;

	if (ddi < INTEL_DDI_MGR_NUM_PORTS)
		record_ddi_route(&hw_state->route[ddi], TC_PORT_NONE);

	if (hw_state->pool_ddi_mask & BIT(ddi) &&
	    intel_de_read(display, DDI_BUF_CTL(ddi_to_port(ddi))) & DDI_BUF_CTL_ENABLE)
		hw_state->busy_ddi_mask |= BIT(ddi);

	if (!consumer_is_dpalt_legacy(consumer))
		return 0;

	phy = consumer_to_phy(consumer);

	/* The IOM also assigns PHY consumers for DP-alt ports. */
	if (!(mgr->legacy_phy_mask & BIT(phy)))
		return 0;

	/* Reject legacy PHY assignments outside the supported DDI range. */
	if (ddi >= INTEL_DDI_MGR_NUM_PORTS) {
		drm_err(display->drm,
			"DDIMGR: Legacy PHY%d assigned to unsupported DDI%d (resources 0x%08x)\n",
			phy, ddi, hw_state->current_resources);

		return -EOPNOTSUPP;
	}

	/* Reject duplicate legacy PHY assignments. */
	if (hw_state->assigned_phy_mask & BIT(phy)) {
		drm_err(display->drm,
			"DDIMGR: Duplicate legacy PHY%d assignment at DDI%d (resources 0x%08x)\n",
			phy, ddi, hw_state->current_resources);

		return -EINVAL;
	}

	record_ddi_route(&hw_state->route[ddi], phy);
	hw_state->assigned_phy_mask |= BIT(phy);

	return 0;
}

static int ddi_mgr_readout_hw_state(struct intel_display *display,
				    struct intel_ddi_hw_state *hw_state)
{
	int ddi, err;

	hw_state->current_resources = intel_de_read(display, IOM_DP_RESOURCE_MNG);
	hw_state->inherited_resources = hw_state->current_resources;
	hw_state->pool_ddi_mask = resource_to_pool_ddi_mask(hw_state->current_resources);
	hw_state->busy_ddi_mask = 0;
	hw_state->assigned_phy_mask = 0;
	hw_state->allocated_ddi_mask = 0;

	for (ddi = 0; ddi < IOM_DDI_NUM_SLOTS; ddi++) {
		err = readout_ddi_resource(display, ddi, hw_state);
		if (err)
			return err;
	}

	return 0;
}

static int ddi_mgr_verify_hw_state(struct intel_display *display,
				   const struct intel_ddi_hw_state *hw_state)
{
	int err;

	verify_ddi_counts(display, hw_state->current_resources);

	err = verify_ddi_pool(display, hw_state->pool_ddi_mask);
	if (err)
		return err;

	return verify_ddc_pins(display, hw_state->pool_ddi_mask);
}

static void ddi_mgr_store_hw_state(struct intel_ddi_mgr *mgr,
				   const struct intel_ddi_hw_state *hw_state)
{
	memcpy(mgr->route, hw_state->route, sizeof(mgr->route));
	mgr->pool_ddi_mask = hw_state->pool_ddi_mask;
}

static int find_free_ddi(u8 ddi_mask, u32 resources)
{
	int ddi;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		if (!(ddi_mask & BIT(ddi)))
			continue;

		if (IOM_DDI_CONSUMER_GET(resources, ddi) != IOM_DDI_CONSUMER_FREE)
			continue;

		return ddi;
	}

	return -ENOSPC;
}

static int legacy_phy_to_ddi(const struct intel_ddi_route *routes, enum tc_port phy)
{
	int ddi;

	if (!routes)
		return -ENOENT;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		if (routes[ddi].mode != INTEL_DDI_ROUTE_LEGACY)
			continue;

		if (routes[ddi].phy == phy)
			return ddi;
	}

	return -ENOENT;
}

static void allocate_ddi_for_phy(struct intel_ddi_hw_state *hw_state,
				 u8 ddi_mask, enum tc_port phy)
{
	int ddi;

	ddi = find_free_ddi(ddi_mask, hw_state->current_resources);
	if (ddi < 0)
		return;

	hw_state->current_resources |= IOM_DDI_CONSUMER(ddi, IOM_DDI_CONSUMER_PHY(phy));
	hw_state->allocated_ddi_mask |= BIT(ddi);
	hw_state->assigned_phy_mask |= BIT(phy);
	record_ddi_route(&hw_state->route[ddi], phy);
}

enum ddi_alloc_policy {
	ALLOC_PREVIOUS_DDI,
	ALLOC_IDENTITY_DDI,
	ALLOC_ANY_FREE_DDI,

	ALLOC_POLICY_NUM,
};

static u8 ddi_mask_for_policy(const struct intel_ddi_hw_state *hw_state,
			      enum ddi_alloc_policy policy,
			      const struct intel_ddi_route *old_routes, enum tc_port phy)
{
	u8 ddi_mask = hw_state->pool_ddi_mask & ~hw_state->busy_ddi_mask;
	int old_ddi;

	switch (policy) {
	case ALLOC_PREVIOUS_DDI:
		old_ddi = legacy_phy_to_ddi(old_routes, phy);
		if (old_ddi < 0)
			return 0;

		ddi_mask &= BIT(old_ddi);

		break;
	case ALLOC_IDENTITY_DDI:
		ddi_mask &= BIT(phy);

		break;
	case ALLOC_ANY_FREE_DDI:
		break;
	default:
		MISSING_CASE(policy);
		return 0;
	}

	return ddi_mask;
}

static int allocate_legacy_ddis(struct intel_display *display,
				struct intel_ddi_hw_state *hw_state,
				const struct intel_ddi_route *old_routes)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	u32 old_resources = hw_state->current_resources;
	u32 new_resources;
	enum ddi_alloc_policy policy;
	enum tc_port phy;
	u32 readback;

	/* Prefer previous DDIs, then identity mappings, then any remaining DDI. */
	for (policy = ALLOC_PREVIOUS_DDI; policy < ALLOC_POLICY_NUM; policy++) {
		for (phy = TC_PORT_1; phy < INTEL_DDI_MGR_NUM_PORTS; phy++) {
			u8 ddi_mask;

			if (!(mgr->legacy_phy_mask & BIT(phy)))
				continue;

			if (hw_state->assigned_phy_mask & BIT(phy))
				continue;

			ddi_mask = ddi_mask_for_policy(hw_state, policy, old_routes, phy);
			allocate_ddi_for_phy(hw_state, ddi_mask, phy);
		}
	}

	new_resources = hw_state->current_resources;
	if (new_resources == old_resources)
		return 0;

	intel_de_write(display, IOM_DP_RESOURCE_MNG, new_resources);
	readback = intel_de_read(display, IOM_DP_RESOURCE_MNG);
	hw_state->current_resources = readback;
	if (readback != new_resources) {
		drm_err(display->drm, "DDIMGR: DDI write mismatch: requested 0x%08x, read 0x%08x\n",
			new_resources, readback);

		return -EIO;
	}

	return 0;
}

static int configure_ddi_resources(struct intel_display *display,
				   const struct intel_ddi_route *old_routes)
{
	struct intel_ddi_hw_state hw_state;
	int err;

	err = iom_lock(display);
	if (err)
		return err;

	err = ddi_mgr_readout_hw_state(display, &hw_state);
	if (err)
		goto out;

	err = ddi_mgr_verify_hw_state(display, &hw_state);
	if (err)
		goto out;

	err = allocate_legacy_ddis(display, &hw_state, old_routes);

	ddi_mgr_store_hw_state(display->ddi_mgr, &hw_state);

out:
	iom_unlock(display);

	print_ddi_resources(display, &hw_state, err);

	return err;
}

/* Update HPD sources before TC initialization or HPD IRQ enable. */
static void update_hpd_sources(struct intel_display *display)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	int ddi;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		const struct intel_ddi_route *route = &mgr->route[ddi];
		enum hpd_pin logical_pin = tc_to_hpd_pin(ddi_to_tc(ddi));
		enum hpd_pin pch_hw_pin = HPD_NONE;
		enum hpd_pin hw_pin = HPD_NONE;

		if (!(mgr->pool_ddi_mask & BIT(ddi)))
			continue;

		if (intel_ddi_mgr_is_enabled(mgr) && route->mode == INTEL_DDI_ROUTE_LEGACY)
			pch_hw_pin = tc_to_hpd_pin(route->phy);
		else
			hw_pin = logical_pin;

		intel_hpd_set_pin_sources(display, logical_pin, hw_pin, pch_hw_pin);
	}
}

static int init_ddi_resource(struct intel_display *display, int ddi)
{
	struct intel_ddi_mgr *mgr = display->ddi_mgr;
	const struct intel_bios_encoder_data *vbt;
	int pin;

	record_ddi_route(&mgr->route[ddi], TC_PORT_NONE);

	vbt = intel_bios_encoder_data_lookup(display, ddi_to_port(ddi));
	if (!vbt || !intel_bios_encoder_supports_dyn_port_over_tc(vbt))
		return 0;

	mgr->legacy_phy_mask |= BIT(ddi);

	if (!intel_bios_encoder_supports_hdmi(vbt) &&
	    !intel_bios_encoder_supports_dvi(vbt))
		return 0;

	mgr->has_hdmi = true;
	pin = intel_bios_hdmi_ddc_pin(vbt);
	if (!pin)
		pin = default_ddc_pin(display, ddi_to_port(ddi));

	if (!intel_gmbus_is_valid_pin(display, pin) || mgr->ddc_pins & BIT(pin)) {
		drm_err(display->drm, "DDIMGR: Invalid/duplicate legacy DDC pin %d\n", pin);

		return -EINVAL;
	}

	mgr->ddc_pin[ddi] = pin;
	mgr->ddc_pins |= BIT(pin);

	return 0;
}

void intel_ddi_mgr_init(struct intel_display *display)
{
	struct intel_ddi_mgr *mgr;
	int ddi, err;

	if (DISPLAY_VER(display) < 35)
		return;

	mgr = drmm_kzalloc(display->drm, sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return;

	display->ddi_mgr = mgr;

	for (ddi = 0; ddi < INTEL_DDI_MGR_NUM_PORTS; ddi++) {
		err = init_ddi_resource(display, ddi);
		if (err)
			goto err;
	}

	if (!mgr->legacy_phy_mask)
		goto out_free;

	err = configure_ddi_resources(display, NULL);
	if (err)
		goto err;

	mgr->enabled = true;
	update_hpd_sources(display);

	return;

err:
	if (mgr->pool_ddi_mask) {
		fallback_to_alt(mgr);
		drm_err(display->drm,
			"DDIMGR: Cannot initialize DDI pool: %d; using ALT mode\n", err);
		update_hpd_sources(display);

		return;
	}

	drm_err(display->drm,
		"DDIMGR: Cannot initialize DDI pool: %d; using default port configuration\n", err);
out_free:
	drmm_kfree(display->drm, mgr);
	display->ddi_mgr = NULL;
}
