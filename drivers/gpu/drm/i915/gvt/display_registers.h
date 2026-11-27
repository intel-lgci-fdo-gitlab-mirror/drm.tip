/* SPDX-License-Identifier: MIT
 *
 * Copyright © 2025 Intel Corporation
 */

#ifndef __DISPLAY_REGISTERS_H__
#define __DISPLAY_REGISTERS_H__

#include "display/intel_gvt_api.h"

/* register includes */
#include "display/bxt_dpio_phy_regs.h"
#include "display/i9xx_plane_regs.h"
#include "display/i9xx_wm_regs.h"
#include "display/intel_audio_regs.h"
#include "display/intel_backlight_regs.h"
#include "display/intel_color_regs.h"
#include "display/intel_crt_regs.h"
#include "display/intel_cursor_regs.h"
#include "display/intel_display_regs.h"
#include "display/intel_dmc_regs.h"
#include "display/intel_dp_aux_regs.h"
#include "display/intel_fbc_regs.h"
#include "display/intel_fdi_regs.h"
#include "display/intel_gmbus_regs.h"
#include "display/intel_lvds_regs.h"
#include "display/intel_pfit_regs.h"
#include "display/intel_pps_regs.h"
#include "display/intel_psr_regs.h"
#include "display/intel_sbi_regs.h"
#include "display/intel_sprite_regs.h"
#include "display/intel_vga_regs.h"
#include "display/skl_universal_plane_regs.h"
#include "display/skl_watermark_regs.h"
#include "display/vlv_dsi_pll_regs.h"

/* some duplicate definitions to avoid extra display includes */
#define DPLL_ID_SKL_DPLL0 0
#define DPLL_ID_WRPLL1 0
#define DPLL_ID_WRPLL2 1
#define INTEL_FBC_A 0
#define DPIO_CH0 0
#define DPIO_CH1 1
#define DPIO_PHY0 0
#define DPIO_PHY1 1

#define DISPLAY_MMIO_BASE(display) \
	intel_display_device_mmio_base((display))

/*
 * #FIXME:
 * TRANSCONF() uses pipe-based addressing via _MMIO_PIPE2().
 * Some GVT call sites pass enum transcoder instead of enum pipe.
 * Cast the argument to enum pipe for now since TRANSCODER_A..D map
 * 1:1 to PIPE_A..D.
 * TRANSCODER_EDP is an exception, the cast preserves the existing
 * behaviour but this needs to be handled later either by using the
 * correct pipe or by switching TRANSCONF() to use _MMIO_TRANS2().
 */
#define INTEL_DISPLAY_DEVICE_PIPE_OFFSET(display, idx) \
	intel_display_device_pipe_offset((display), (enum pipe)(idx))

#define INTEL_DISPLAY_DEVICE_TRANS_OFFSET(display, trans) \
	intel_display_device_trans_offset((display), (trans))

#define INTEL_DISPLAY_DEVICE_CURSOR_OFFSET(display, pipe) \
	intel_display_device_cursor_offset((display), (pipe))

#define gvt_for_each_pipe(display, __p) \
	for ((__p) = PIPE_A; (__p) < I915_MAX_PIPES; (__p)++) \
		for_each_if(intel_display_device_pipe_valid((display), (__p)))

#endif /* __DISPLAY_REGISTERS_H__ */
