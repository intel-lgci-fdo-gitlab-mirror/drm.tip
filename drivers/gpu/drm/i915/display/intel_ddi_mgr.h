/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __INTEL_DDI_MGR_H__
#define __INTEL_DDI_MGR_H__

#include <linux/types.h>

enum port;

struct intel_bios_encoder_data;
struct intel_connector;
struct intel_display;
struct intel_encoder;

void intel_ddi_mgr_init(struct intel_display *display);
void intel_ddi_mgr_suspend(struct intel_display *display);
void intel_ddi_mgr_resume(struct intel_display *display, bool suspend_failed);
void intel_ddi_mgr_verify_state(struct intel_display *display);

bool intel_ddi_mgr_is_pool_ddi(struct intel_display *display, enum port ddi);
bool intel_ddi_mgr_is_ddi_disabled(struct intel_display *display, enum port ddi);
bool intel_ddi_mgr_pool_supports_hdmi(struct intel_display *display);
bool intel_ddi_mgr_is_legacy_pool_encoder(struct intel_encoder *encoder);
bool intel_ddi_mgr_encoder_has_lane_reversal(struct intel_encoder *encoder);
bool intel_ddi_mgr_aux_available(struct intel_encoder *encoder);
bool intel_ddi_mgr_connector_available(struct intel_connector *connector);
u8 intel_ddi_mgr_ddc_pin_for_ddi(struct intel_display *display, enum port ddi);
const struct intel_bios_encoder_data *
intel_ddi_mgr_get_devdata_for_ddi(struct intel_display *display, enum port ddi);

#endif
