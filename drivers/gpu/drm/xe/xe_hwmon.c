// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <linux/units.h>

#include <drm/drm_managed.h>
#include "regs/xe_gt_regs.h"
#include "regs/xe_mchbar_regs.h"
#include "regs/xe_pcode_regs.h"
#include "xe_device.h"
#include "xe_hwmon.h"
#include "xe_log.h"
#include "xe_mmio.h"
#include "xe_pcode.h"
#include "xe_pcode_api.h"
#include "xe_sriov.h"
#include "xe_pm.h"
#include "xe_printk.h"
#include "xe_vsec.h"
#include "regs/xe_pmt.h"

enum xe_hwmon_reg {
	REG_TEMP,
	REG_PKG_RAPL_LIMIT,
	REG_PKG_POWER_SKU,
	REG_PKG_POWER_SKU_UNIT,
	REG_GT_PERF_STATUS,
	REG_PKG_ENERGY_STATUS,
	REG_FAN_SPEED,
};

enum xe_hwmon_reg_operation {
	REG_READ32,
	REG_RMW32,
	REG_READ64,
};

#define MAX_VRAM_CHANNELS	80
#define FIXED_VRAM_CHANNELS	16

/* Each MSU enable bit maps to one VRAM subsystem of 4 channels. */
#define VRAM_CHANNELS_PER_MSU	4

enum xe_hwmon_channel {
	CHANNEL_CARD,
	CHANNEL_PKG,
	CHANNEL_VRAM,
	CHANNEL_MCTRL,
	CHANNEL_PCIE,
	CHANNEL_VRAM_N,
	/* Compile-time upper bound; actual channel count is hwmon->temp.vram_count */
	CHANNEL_VRAM_N_MAX = CHANNEL_VRAM_N + MAX_VRAM_CHANNELS - 1,
	CHANNEL_MAX,
};

enum xe_fan_channel {
	FAN_1,
	FAN_2,
	FAN_3,
	FAN_MAX,
};

enum xe_temp_limit {
	TEMP_LIMIT_PKG_SHUTDOWN,
	TEMP_LIMIT_PKG_CRIT,
	TEMP_LIMIT_MEM_SHUTDOWN,
	TEMP_LIMIT_PKG_MAX,
	TEMP_LIMIT_MEM_CRIT,
	TEMP_LIMIT_MAX
};

/* Attribute index for powerX_xxx_interval sysfs entries */
enum sensor_attr_power {
	SENSOR_INDEX_PSYS_PL1,
	SENSOR_INDEX_PKG_PL1,
	SENSOR_INDEX_PSYS_PL2,
	SENSOR_INDEX_PKG_PL2,
};

/*
 * For platforms that support mailbox commands for power limits, REG_PKG_POWER_SKU_UNIT is
 * not supported and below are SKU units to be used.
 */
#define PWR_UNIT	0x3
#define ENERGY_UNIT	0xe
#define TIME_UNIT	0xa

/*
 * SF_* - scale factors for particular quantities according to hwmon spec.
 */
#define SF_POWER	1000000		/* microwatts */
#define SF_CURR		1000		/* milliamperes */
#define SF_VOLTAGE	1000		/* millivolts */
#define SF_ENERGY	1000000		/* microjoules */
#define SF_TIME		1000		/* milliseconds */

/*
 * PL*_HWMON_ATTR - mapping of hardware power limits to corresponding hwmon power attribute.
 */
#define PL1_HWMON_ATTR	hwmon_power_max
#define PL2_HWMON_ATTR	hwmon_power_cap

#define PWR_ATTR_TO_STR(attr)	(((attr) == hwmon_power_max) ? "PL1" : "PL2")

/* Sensor byte indices in READ_THERMAL_DATA output for BMG. */
#define TEMP_INDEX_MCTRL	2
#define TEMP_INDEX_PCIE		10
#define BMG_G21_TEMP_INDEX_PCIE	8

/* Sensor byte indices in READ_THERMAL_DATA output for CRI. */
#define CRI_TEMP_INDEX_MCTRL	16
#define CRI_TEMP_MCTRL_COUNT	4
#define CRI_TEMP_INDEX_PCIE	20

/* Mailbox sentinel reported for an unavailable temperature sensor */
#define TEMP_MBX_INVALID	0xFF

/* Mailbox temperature is sign-magnitude: bit 7 sign, bits 6:0 magnitude. */
#define TEMP_MBX_SIGN		BIT(7)
#define TEMP_MBX_MAG		GENMASK(6, 0)

/* Maximum characters in hwmon label name */
#define MAX_LABEL_SIZE		16

/**
 * struct xe_hwmon_energy_info - to accumulate energy
 */
struct xe_hwmon_energy_info {
	/** @reg_val_prev: previous energy reg val */
	u32 reg_val_prev;
	/** @accum_energy: accumulated energy */
	long accum_energy;
};

/**
 * DOC: Xe fan control
 *
 * Xe fan control is exposed through the hwmon pwm and fan interfaces when the
 * platform advertises PCODE-backed fan management support.
 *
 * The implementation tracks two fan tables per fan:
 *
 * - A stock table read from firmware through PCODE.
 * - A user table cached in the driver and programmed back through PCODE.
 *
 * The pwmN_enable sysfs value selects how those tables are used:
 *
 * - 0 (``XE_FAN_PWM_FULL_SPEED``) programs a two-point user table at 100
 *   percent duty cycle as a full-speed override.
 * - 1 (``XE_FAN_PWM_MANUAL_USER_TABLE``) enables the user table for manual
 *   control.
 * - 2 (``XE_FAN_PWM_AUTO_STOCK_TABLE``) restores automatic control using the
 *   stock firmware table.
 *
 * The fan-control sysfs files are:
 *
 * - ``fanN_input`` reports the measured fan speed in RPM.
 * - ``fanN_max`` reports the maximum supported fan speed in RPM.
 * - ``pwmN`` reads or sets the manual PWM duty cycle in the hwmon 0..255
 *   range.
 * - ``pwmN_enable`` selects the mode described above.
 * - ``pwmN_auto_pointM_temp`` exposes the temperature threshold for user-table
 *   point ``M`` in millidegrees Celsius.
 * - ``pwmN_auto_pointM_pwm`` exposes the PWM value for user-table point ``M``
 *   in the hwmon 0..255 range.
 *
 * Writes to ``pwmN``, ``pwmN_auto_pointM_temp`` and ``pwmN_auto_pointM_pwm``
 * are accepted only while ``pwmN_enable`` is 1; otherwise they fail with
 * -EINVAL. Firmware stores duty cycle in percent, so PWM values read back
 * rounded to the nearest percent step. PWM values below the minimum duty cycle
 * reported by firmware for that fan are raised to it, except a curve point PWM
 * of 0, which is kept for the end marker.
 *
 * Writes to ``pwmN_auto_pointM_temp`` and ``pwmN_auto_pointM_pwm`` are staged
 * in the driver, so intermediate writes never reach firmware as a partial
 * curve. The staged curve is committed once points 1..K all have both a
 * temperature and a PWM value and either point K+1 is set to (0, 0) or K is
 * the last supported point. The (0, 0) end marker is not part of the curve.
 * A curve must have at least two points, and temperature and PWM must not
 * decrease from one point to the next; otherwise the completing write fails
 * with -EINVAL.
 *
 * Writes to ``pwmN`` replace the user table with a two-point table at a
 * constant duty cycle.
 */

/**
 * enum xe_fan_pwm_enable_mode - fan PWM enable modes
 */
enum xe_fan_pwm_enable_mode {
	/** @XE_FAN_PWM_FULL_SPEED: force all user table points to full speed */
	XE_FAN_PWM_FULL_SPEED = 0,
	/** @XE_FAN_PWM_MANUAL_USER_TABLE: use the driver-managed user table */
	XE_FAN_PWM_MANUAL_USER_TABLE = 1,
	/** @XE_FAN_PWM_AUTO_STOCK_TABLE: use the firmware stock table */
	XE_FAN_PWM_AUTO_STOCK_TABLE = 2,
};

/**
 * enum fan_table_type - fan table types tracked per fan
 */
enum fan_table_type {
	/** @STOCK_FAN_TABLE: firmware-provided fan table */
	STOCK_FAN_TABLE,
	/** @USER_FAN_TABLE: user fan table written through sysfs */
	USER_FAN_TABLE,
	/** @FAN_TABLE_MAX: number of fan tables tracked per fan */
	FAN_TABLE_MAX,
};

/* Check if a fan is enabled based on the fan_enable_mask got from FSC_READ_NUM_FANS. */
#define IS_FAN_ENABLED(hwmon, fan) ((hwmon)->fan_enable_mask & BIT(fan))

/* Maximum number of fan control points supported by each fan. */
#define MAX_FAN_CONTROL_POINTS	10

/* Minimum number of points required for a user fan table. */
#define MIN_FAN_CONTROL_POINTS	2

/* pwmN_auto_pointM_temp and pwmN_auto_pointM_pwm for every curve point of a fan. */
#define XE_CURVE_ATTRS_PER_FAN	(2 * MAX_FAN_CONTROL_POINTS)
#define XE_CURVE_ATTRS		(FAN_MAX * XE_CURVE_ATTRS_PER_FAN)
#define XE_CURVE_ATTR_NAME_LEN	sizeof("pwmN_auto_pointNN_temp")

/* Fan control point index: bit31 selects fan table type; bits30:0 control point index. */
#define FCP_INDEX(table_type, point_num) (((u32)(table_type) << 31) | \
						((u32)(point_num) & GENMASK(30, 0)))

/**
 * struct xe_hwmon_fan_info - cached fan telemetry and control state
 *
 * Each fan keeps the latest tachometer sampling state along with two fan
 * tables: the stock table discovered from firmware and the user table managed
 * by hwmon sysfs writes.
 */
struct xe_hwmon_fan_info {
	/** @reg_val_prev: previous fan reg val */
	u32 reg_val_prev;
	/** @time_prev: previous timestamp */
	u64 time_prev;
	/** @fan_table: fan control tables */
	struct fan_table {
		/** @fan_table.point_count: number of fan control points */
		u8 point_count;
		/** @fan_table.fcp: array of fan control points */
		struct fan_control_point {
			/** @fan_table.fcp.temp: temperature in degree celsius */
			u8 temp;
			/** @fan_table.fcp.speed: fan speed in percentage */
			u8 speed;
		} fcp[MAX_FAN_CONTROL_POINTS];
		/** @fan_table.staged_fcp: curve supplied via sysfs but not yet committed */
		struct fan_control_point staged_fcp[MAX_FAN_CONTROL_POINTS];
		/** @fan_table.temp_staged: bitmask of temperature fields supplied via sysfs */
		u16 temp_staged;
		/** @fan_table.speed_staged: bitmask of speed fields supplied via sysfs */
		u16 speed_staged;
	} fan_table[FAN_TABLE_MAX];
	/** @max_rps: maximum fan RPS */
	u32 max_rps;
	/** @min_pwm: minimum fan PWM */
	u32 min_pwm;
	/** @pwm_enable_mode: current xe_fan_pwm_enable_mode */
	u8 pwm_enable_mode;
	/** @is_full_speed: flag indicating if fan is in full speed */
	bool is_full_speed;
	/** @restore_user_table: restore user fan table on next resume */
	bool restore_user_table;
};

/**
 * struct xe_hwmon_thermal_info - to store temperature data
 */
struct xe_hwmon_thermal_info {
	union {
		/** @limit: temperatures limits */
		u8 limit[TEMP_LIMIT_MAX];
		/** @data: temperature limits as little-endian dwords */
		__le32 data[DIV_ROUND_UP(TEMP_LIMIT_MAX, sizeof(u32))];
	};
	/** @count: temperature sensors count from READ_THERMAL_CONFIG */
	u8 count;
	/** @vram_count: exclusive upper bound for VRAM temperature channel indices */
	u8 vram_count;
	/** @available: temperature sensor availability cached at registration */
	bool available[CHANNEL_MAX];
	/** @msu_mask: VRAM subsystem enable mask read from MMIO */
	u32 msu_mask;
	union {
		/** @value: per-sensor raw mailbox temperature; bit7=sign, bits6:0=magnitude */
		u8 value[U8_MAX + 1];
		/** @dword: sensor values as little-endian dwords, u32-aligned for pcode reads */
		__le32 dword[DIV_ROUND_UP(U8_MAX + 1, sizeof(u32))];
	};
	/** @vram_label: vram label names, dynamically allocated based on vram_count */
	char (*vram_label)[MAX_LABEL_SIZE];
};

/**
 * struct xe_hwmon - xe hwmon data structure
 */
struct xe_hwmon {
	/** @hwmon_dev: hwmon device for xe */
	struct device *hwmon_dev;
	/** @xe: Xe device */
	struct xe_device *xe;
	/** @hwmon_lock: lock for rw attributes*/
	struct mutex hwmon_lock;
	/** @scl_shift_power: pkg power unit */
	int scl_shift_power;
	/** @scl_shift_energy: pkg energy unit */
	int scl_shift_energy;
	/** @scl_shift_time: pkg time unit */
	int scl_shift_time;
	/** @ei: Energy info for energyN_input */
	struct xe_hwmon_energy_info ei[CHANNEL_MAX];
	/** @fan_enable_mask: bitmask of available fans */
	u32 fan_enable_mask;
	/** @fi: Fan info for fanN_input */
	struct xe_hwmon_fan_info fi[FAN_MAX];
	/** @read_power_limit_from_pcode: pcode accepted power limits write for the channel */
	bool read_power_limit_from_pcode[CHANNEL_MAX];
	/** @pl1_on_boot: power limit PL1 on boot */
	u32 pl1_on_boot[CHANNEL_MAX];
	/** @pl2_on_boot: power limit PL2 on boot */
	u32 pl2_on_boot[CHANNEL_MAX];
	/** @temp: Temperature info */
	struct xe_hwmon_thermal_info temp;
	/** @curve_sda: fan curve sensor attributes */
	struct sensor_device_attribute curve_sda[XE_CURVE_ATTRS];
	/** @curve_names: names of the fan curve sensor attributes */
	char curve_names[XE_CURVE_ATTRS][XE_CURVE_ATTR_NAME_LEN];
	/** @curve_attrs: NULL-terminated list of fan curve attributes */
	struct attribute *curve_attrs[XE_CURVE_ATTRS + 1];
	/** @curve_group: fan curve attribute group */
	struct attribute_group curve_group;
	/** @groups: NULL-terminated list of extra attribute groups */
	const struct attribute_group *groups[3];
};

static inline int prepare_power_limit_param2(const struct xe_hwmon *hwmon, int channel)
{
	if (hwmon->read_power_limit_from_pcode[channel]) {
		if (hwmon->xe->info.platform >= XE_CRESCENTISLAND)
			return READ_PL_ACCEPTED;
		else
			return READ_PL_FROM_PCODE;
	} else {
		return READ_PL_FROM_FW;
	}
}

static int xe_hwmon_pcode_read_power_limit(const struct xe_hwmon *hwmon, u32 attr, int channel,
					   u32 *uval)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	u32 val0 = 0, val1 = 0;
	int ret = 0;

	ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_POWER_SETUP,
						  (channel == CHANNEL_CARD) ?
						  READ_PSYSGPU_POWER_LIMIT :
						  READ_PACKAGE_POWER_LIMIT,
						  prepare_power_limit_param2(hwmon, channel)),
			    &val0, &val1);

	if (ret) {
		drm_dbg(&hwmon->xe->drm, "read failed ch %d val0 0x%08x, val1 0x%08x, ret %d\n",
			channel, val0, val1, ret);
		*uval = 0;
		return ret;
	}

	/* return the value only if limit is enabled */
	if (attr == PL1_HWMON_ATTR)
		*uval = (val0 & PWR_LIM_EN) ? val0 : 0;
	else if (attr == PL2_HWMON_ATTR)
		*uval = (val1 & PWR_LIM_EN) ? val1 : 0;
	else if (attr == hwmon_power_label)
		*uval = (val0 | val1) & PWR_LIM_EN;
	else
		*uval = 0;

	return ret;
}

static int xe_hwmon_pcode_rmw_power_limit(struct xe_hwmon *hwmon, u32 attr, u8 channel,
					  u32 clr, u32 set)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	u32 val0 = 0, val1 = 0;
	int ret = 0;

	ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_POWER_SETUP,
						  (channel == CHANNEL_CARD) ?
						  READ_PSYSGPU_POWER_LIMIT :
						  READ_PACKAGE_POWER_LIMIT,
						  prepare_power_limit_param2(hwmon, channel)),
			    &val0, &val1);
	if (ret) {
		drm_err(&hwmon->xe->drm, "read failed ch %d val0 0x%08x, val1 0x%08x, ret %d\n",
			channel, val0, val1, ret);
		return ret;
	}

	if (attr == PL1_HWMON_ATTR)
		val0 = (val0 & ~clr) | set;
	else if (attr == PL2_HWMON_ATTR)
		val1 = (val1 & ~clr) | set;
	else
		return -EIO;

	ret = xe_pcode_write64_timeout(root_tile, PCODE_MBOX(PCODE_POWER_SETUP,
							     (channel == CHANNEL_CARD) ?
							     WRITE_PSYSGPU_POWER_LIMIT :
							     WRITE_PACKAGE_POWER_LIMIT, 0),
							     val0, val1, PCODE_DEFAULT_TIMEOUT_MS);
	if (ret) {
		drm_dbg(&hwmon->xe->drm, "write failed ch %d val0 0x%08x, val1 0x%08x, ret %d\n",
			channel, val0, val1, ret);
		return ret;
	}

	/* Read from pcode once it holds a driver-written limit */
	if (!hwmon->read_power_limit_from_pcode[channel])
		hwmon->read_power_limit_from_pcode[channel] = true;

	return 0;
}

static int xe_hwmon_pcode_set_default_power_limits(struct xe_hwmon *hwmon)
{
	int ret = 0;

	ret |= xe_hwmon_pcode_rmw_power_limit(hwmon, PL1_HWMON_ATTR, CHANNEL_CARD,
						       PWR_LIM | PWR_LIM_TIME,
						       hwmon->pl1_on_boot[CHANNEL_CARD]);
	ret |= xe_hwmon_pcode_rmw_power_limit(hwmon, PL1_HWMON_ATTR, CHANNEL_PKG,
						       PWR_LIM | PWR_LIM_TIME,
						       hwmon->pl1_on_boot[CHANNEL_PKG]);
	ret |= xe_hwmon_pcode_rmw_power_limit(hwmon, PL2_HWMON_ATTR, CHANNEL_CARD,
						       PWR_LIM | PWR_LIM_TIME,
						       hwmon->pl2_on_boot[CHANNEL_CARD]);
	ret |= xe_hwmon_pcode_rmw_power_limit(hwmon, PL2_HWMON_ATTR, CHANNEL_PKG,
						       PWR_LIM | PWR_LIM_TIME,
						       hwmon->pl2_on_boot[CHANNEL_PKG]);

	return ret;
}

static bool xe_hwmon_vram_channel_enabled(const struct xe_hwmon *hwmon, int index)
{
	if (hwmon->xe->info.platform == XE_CRESCENTISLAND)
		return hwmon->temp.msu_mask & BIT(index / VRAM_CHANNELS_PER_MSU);

	return true;
}

static struct xe_reg xe_hwmon_get_reg(struct xe_hwmon *hwmon, enum xe_hwmon_reg hwmon_reg,
				      int channel)
{
	struct xe_device *xe = hwmon->xe;

	switch (hwmon_reg) {
	case REG_TEMP:
		if (xe->info.platform == XE_CRESCENTISLAND) {
			if (channel == CHANNEL_PKG)
				return CRI_PACKAGE_TEMPERATURE;
			else if (channel == CHANNEL_VRAM)
				return CRI_VRAM_TEMPERATURE;
			else if (in_range(channel, CHANNEL_VRAM_N, hwmon->temp.vram_count) &&
				 xe_hwmon_vram_channel_enabled(hwmon, channel - CHANNEL_VRAM_N))
				return CRI_VRAM_TEMPERATURE_N(channel - CHANNEL_VRAM_N);
		} else if (xe->info.platform == XE_BATTLEMAGE) {
			if (channel == CHANNEL_PKG)
				return BMG_PACKAGE_TEMPERATURE;
			else if (channel == CHANNEL_VRAM)
				return BMG_VRAM_TEMPERATURE;
			else if (in_range(channel, CHANNEL_VRAM_N, hwmon->temp.vram_count) &&
				 xe_hwmon_vram_channel_enabled(hwmon, channel - CHANNEL_VRAM_N))
				return BMG_VRAM_TEMPERATURE_N(channel - CHANNEL_VRAM_N);
		} else if (xe->info.platform == XE_DG2) {
			if (channel == CHANNEL_PKG)
				return PCU_CR_PACKAGE_TEMPERATURE;
			else if (channel == CHANNEL_VRAM)
				return BMG_VRAM_TEMPERATURE;
		}
		break;
	case REG_PKG_RAPL_LIMIT:
		if (xe->info.platform == XE_PVC && channel == CHANNEL_PKG)
			return PVC_GT0_PACKAGE_RAPL_LIMIT;
		else if ((xe->info.platform == XE_DG2) && (channel == CHANNEL_PKG))
			return PCU_CR_PACKAGE_RAPL_LIMIT;
		break;
	case REG_PKG_POWER_SKU:
		if (xe->info.platform == XE_PVC && channel == CHANNEL_PKG)
			return PVC_GT0_PACKAGE_POWER_SKU;
		else if ((xe->info.platform == XE_DG2) && (channel == CHANNEL_PKG))
			return PCU_CR_PACKAGE_POWER_SKU;
		break;
	case REG_PKG_POWER_SKU_UNIT:
		if (xe->info.platform == XE_PVC)
			return PVC_GT0_PACKAGE_POWER_SKU_UNIT;
		else if (xe->info.platform == XE_DG2)
			return PCU_CR_PACKAGE_POWER_SKU_UNIT;
		break;
	case REG_GT_PERF_STATUS:
		if (xe->info.platform == XE_DG2 && channel == CHANNEL_PKG)
			return GT_PERF_STATUS;
		break;
	case REG_PKG_ENERGY_STATUS:
		if (xe->info.platform == XE_CRESCENTISLAND) {
			if (channel == CHANNEL_CARD)
				return CRI_PLATFORM_ENERGY_STATUS;
			else if (channel == CHANNEL_PKG)
				return CRI_PACKAGE_ENERGY_STATUS;
		} else if (xe->info.platform == XE_PVC && channel == CHANNEL_PKG) {
			return PVC_GT0_PLATFORM_ENERGY_STATUS;
		} else if ((xe->info.platform == XE_DG2) && (channel == CHANNEL_PKG)) {
			return PCU_CR_PACKAGE_ENERGY_STATUS;
		}
		break;
	case REG_FAN_SPEED:
		if (channel == FAN_1)
			return BMG_FAN_1_SPEED;
		else if (channel == FAN_2)
			return BMG_FAN_2_SPEED;
		else if (channel == FAN_3)
			return BMG_FAN_3_SPEED;
		break;
	default:
		drm_warn(&xe->drm, "Unknown xe hwmon reg id: %d\n", hwmon_reg);
		break;
	}

	return XE_REG(0);
}

#define PL_DISABLE 0

/*
 * HW allows arbitrary PL1 limits to be set but silently clamps these values to
 * "typical but not guaranteed" min/max values in REG_PKG_POWER_SKU. Follow the
 * same pattern for sysfs, allow arbitrary PL1 limits to be set but display
 * clamped values when read.
 */
static int xe_hwmon_power_max_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *value)
{
	u32 reg_val = 0;
	struct xe_device *xe = hwmon->xe;
	struct xe_reg rapl_limit, pkg_power_sku;
	struct xe_mmio *mmio = xe_root_tile_mmio(xe);
	int ret = 0;

	mutex_lock(&hwmon->hwmon_lock);

	if (hwmon->xe->info.has_mbx_power_limits) {
		ret = xe_hwmon_pcode_read_power_limit(hwmon, attr, channel, &reg_val);
		if (ret)
			goto unlock;
	} else {
		rapl_limit = xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel);
		pkg_power_sku = xe_hwmon_get_reg(hwmon, REG_PKG_POWER_SKU, channel);
		reg_val = xe_mmio_read32(mmio, rapl_limit);
	}

	/* Check if PL limits are disabled. */
	if (!(reg_val & PWR_LIM_EN)) {
		*value = PL_DISABLE;
		drm_info(&hwmon->xe->drm, "%s disabled for channel %d, val 0x%08x\n",
			 PWR_ATTR_TO_STR(attr), channel, reg_val);
		goto unlock;
	}

	reg_val = REG_FIELD_GET(PWR_LIM_VAL, reg_val);
	*value = mul_u32_u32(reg_val, SF_POWER) >> hwmon->scl_shift_power;

	/* For platforms with mailbox power limit support clamping would be done by pcode. */
	if (!hwmon->xe->info.has_mbx_power_limits) {
		u64 pkg_pwr, min, max;

		pkg_pwr = xe_mmio_read64_2x32(mmio, pkg_power_sku);
		min = REG_FIELD_GET(PKG_MIN_PWR, pkg_pwr);
		max = REG_FIELD_GET(PKG_MAX_PWR, pkg_pwr);
		min = mul_u64_u32_shr(min, SF_POWER, hwmon->scl_shift_power);
		max = mul_u64_u32_shr(max, SF_POWER, hwmon->scl_shift_power);
		if (min && max)
			*value = clamp_t(u64, *value, min, max);
	}
unlock:
	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static int xe_hwmon_power_max_write(struct xe_hwmon *hwmon, u32 attr, int channel, long value)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	int ret = 0;
	u32 reg_val, max;
	struct xe_reg rapl_limit;
	u64 max_supp_power_limit = 0;

	mutex_lock(&hwmon->hwmon_lock);

	rapl_limit = xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel);

	/* Disable Power Limit and verify, as limit cannot be disabled on all platforms. */
	if (value == PL_DISABLE) {
		if (hwmon->xe->info.has_mbx_power_limits) {
			drm_dbg(&hwmon->xe->drm, "disabling %s on channel %d\n",
				PWR_ATTR_TO_STR(attr), channel);
			ret = xe_hwmon_pcode_rmw_power_limit(hwmon, attr, channel, PWR_LIM_EN, 0);
			if (ret)
				goto unlock;
			ret = xe_hwmon_pcode_read_power_limit(hwmon, attr, channel, &reg_val);
			if (ret)
				goto unlock;
		} else {
			reg_val = xe_mmio_rmw32(mmio, rapl_limit, PWR_LIM_EN, 0);
			reg_val = xe_mmio_read32(mmio, rapl_limit);
		}

		if (reg_val & PWR_LIM_EN) {
			drm_warn(&hwmon->xe->drm, "Power limit disable is not supported!\n");
			ret = -EOPNOTSUPP;
		}
		goto unlock;
	}

	/*
	 * If the sysfs value exceeds the maximum pcode supported power limit value, clamp it to
	 * the supported maximum (U12.3 format).
	 * This is to avoid truncation during reg_val calculation below and ensure the valid
	 * power limit is sent for pcode which would clamp it to card-supported value.
	 */
	max_supp_power_limit = ((PWR_LIM_VAL) >> hwmon->scl_shift_power) * SF_POWER;
	if (value > max_supp_power_limit) {
		value = max_supp_power_limit;
		drm_info(&hwmon->xe->drm,
			 "Power limit clamped as selected %s exceeds channel %d limit\n",
			 PWR_ATTR_TO_STR(attr), channel);
	}

	/* Computation in 64-bits to avoid overflow. Round to nearest. */
	reg_val = DIV_ROUND_CLOSEST_ULL((u64)value << hwmon->scl_shift_power, SF_POWER);

	/*
	 * Clamp power limit to GPU firmware default as maximum, as an additional protection to
	 * pcode clamp.
	 */
	if (hwmon->xe->info.has_mbx_power_limits) {
		max = (attr == PL1_HWMON_ATTR) ?
		       hwmon->pl1_on_boot[channel] : hwmon->pl2_on_boot[channel];
		max = REG_FIELD_PREP(PWR_LIM_VAL, max);
		if (reg_val > max) {
			reg_val = max;
			drm_dbg(&hwmon->xe->drm,
				"Clamping power limit to GPU firmware default 0x%x\n",
				reg_val);
		}
	}

	reg_val = PWR_LIM_EN | REG_FIELD_PREP(PWR_LIM_VAL, reg_val);

	if (hwmon->xe->info.has_mbx_power_limits)
		ret = xe_hwmon_pcode_rmw_power_limit(hwmon, attr, channel, PWR_LIM, reg_val);
	else
		reg_val = xe_mmio_rmw32(mmio, rapl_limit, PWR_LIM, reg_val);
unlock:
	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static int xe_hwmon_power_rated_max_read(struct xe_hwmon *hwmon, u32 attr, int channel,
					 long *value)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u32 reg_val;
	int ret = 0;

	if (hwmon->xe->info.has_mbx_power_limits) {
		/* PL1 is rated max if supported. */
		ret = xe_hwmon_pcode_read_power_limit(hwmon, PL1_HWMON_ATTR, channel, &reg_val);
		if (ret)
			return ret;
	} else {
		/*
		 * This sysfs file won't be visible if REG_PKG_POWER_SKU is invalid, so valid check
		 * for this register can be skipped.
		 * See xe_hwmon_power_is_visible.
		 */
		struct xe_reg reg = xe_hwmon_get_reg(hwmon, REG_PKG_POWER_SKU, channel);

		reg_val = xe_mmio_read32(mmio, reg);
	}

	reg_val = REG_FIELD_GET(PKG_TDP, reg_val);
	*value = mul_u64_u32_shr(reg_val, SF_POWER, hwmon->scl_shift_power);
	return ret;
}

/*
 * xe_hwmon_energy_get - Obtain energy value
 *
 * The underlying energy hardware register is 32-bits and is subject to
 * overflow. How long before overflow? For example, with an example
 * scaling bit shift of 14 bits (see register *PACKAGE_POWER_SKU_UNIT) and
 * a power draw of 1000 watts, the 32-bit counter will overflow in
 * approximately 4.36 minutes.
 *
 * Examples:
 *    1 watt:  (2^32 >> 14) /    1 W / (60 * 60 * 24) secs/day -> 3 days
 * 1000 watts: (2^32 >> 14) / 1000 W / 60             secs/min -> 4.36 minutes
 *
 * The function significantly increases overflow duration (from 4.36
 * minutes) by accumulating the energy register into a 'long' as allowed by
 * the hwmon API. Using x86_64 128 bit arithmetic (see mul_u64_u32_shr()),
 * a 'long' of 63 bits, SF_ENERGY of 1e6 (~20 bits) and
 * hwmon->scl_shift_energy of 14 bits we have 57 (63 - 20 + 14) bits before
 * energyN_input overflows. This at 1000 W is an overflow duration of 278 years.
 */
static int
xe_hwmon_energy_get(struct xe_hwmon *hwmon, int channel, long *energy)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	struct xe_hwmon_energy_info *ei = &hwmon->ei[channel];
	u32 reg_val;
	int ret = 0;

	/* Energy is supported only for card and pkg */
	if (channel > CHANNEL_PKG) {
		*energy = 0;
		return 0;
	}

	if (hwmon->xe->info.platform == XE_BATTLEMAGE) {
		u64 pmt_val;

		ret = xe_pmt_telem_read(hwmon->xe->drm.dev,
					xe_mmio_read32(mmio, PUNIT_TELEMETRY_GUID),
					&pmt_val, BMG_ENERGY_STATUS_PMT_OFFSET,	sizeof(pmt_val));
		if (ret != sizeof(pmt_val)) {
			drm_warn(&hwmon->xe->drm, "energy read from pmt failed, ret %d\n", ret);
			*energy = 0;
			return ret < 0 ? ret : -EIO;
		}

		if (channel == CHANNEL_PKG)
			reg_val = REG_FIELD_GET64(ENERGY_PKG, pmt_val);
		else
			reg_val = REG_FIELD_GET64(ENERGY_CARD, pmt_val);
	} else {
		reg_val = xe_mmio_read32(mmio, xe_hwmon_get_reg(hwmon, REG_PKG_ENERGY_STATUS,
								channel));
	}

	ei->accum_energy += reg_val - ei->reg_val_prev;
	ei->reg_val_prev = reg_val;

	*energy = mul_u64_u32_shr(ei->accum_energy, SF_ENERGY,
				  hwmon->scl_shift_energy);
	return 0;
}

static ssize_t
xe_hwmon_power_max_interval_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u32 reg_val, x, y, x_w = 2; /* 2 bits */
	u64 tau4, out;
	int channel = (to_sensor_dev_attr(attr)->index % 2) ? CHANNEL_PKG : CHANNEL_CARD;
	u32 power_attr = (to_sensor_dev_attr(attr)->index > 1) ? PL2_HWMON_ATTR : PL1_HWMON_ATTR;

	int ret = 0;

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);

	if (hwmon->xe->info.has_mbx_power_limits) {
		ret = xe_hwmon_pcode_read_power_limit(hwmon, power_attr, channel, &reg_val);
		if (ret) {
			xe_log_err(hwmon->xe, HWMON, ret,
				   "Power interval read fail, ch %d, attr %d, val 0x%08x\n",
				   channel, power_attr, reg_val);
			mutex_unlock(&hwmon->hwmon_lock);
			return ret;
		}
	} else {
		reg_val = xe_mmio_read32(mmio, xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT,
								channel));
	}

	mutex_unlock(&hwmon->hwmon_lock);

	if (hwmon->xe->info.platform >= XE_CRESCENTISLAND) {
		/**
		 * On CRI and newer platforms, the interval encoding changed.
		 * The value is now stored directly in milliseconds as U5.2,
		 * replacing the older 1.x * 2^y representation.
		 * Bits [6:2] hold the integer part and bits [1:0] the fraction,
		 * so convert to milliseconds by extracting integer/fractional parts.
		 * Round fraction value to 1 when it is >= 0.5.
		 */
		reg_val = REG_FIELD_GET(PWR_LIM_TIME, reg_val);
		out = (u64)((reg_val >> 2) + ((reg_val & 0x3) >= 2));
	} else {
		x = REG_FIELD_GET(PWR_LIM_TIME_X, reg_val);
		y = REG_FIELD_GET(PWR_LIM_TIME_Y, reg_val);

		/*
		 * tau = (1 + (x / 4)) * power(2,y), x = bits(23:22), y = bits(21:17)
		 *     = (4 | x) << (y - 2)
		 *
		 * Here (y - 2) ensures a 1.x fixed point representation of 1.x
		 * As x is 2 bits so 1.x can be 1.0, 1.25, 1.50, 1.75
		 *
		 * As y can be < 2, we compute tau4 = (4 | x) << y
		 * and then add 2 when doing the final right shift to account for units
		 */
		tau4 = (u64)((1 << x_w) | x) << y;

		/* val in hwmon interface units (millisec) */
		out = mul_u64_u32_shr(tau4, SF_TIME, hwmon->scl_shift_time + x_w);
	}

	return sysfs_emit(buf, "%llu\n", out);
}

static ssize_t
xe_hwmon_power_max_interval_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u32 x, y, rxy, x_w = 2; /* 2 bits */
	u64 tau4, r, max_win;
	unsigned long val;
	int channel = (to_sensor_dev_attr(attr)->index % 2) ? CHANNEL_PKG : CHANNEL_CARD;
	u32 power_attr = (to_sensor_dev_attr(attr)->index > 1) ? PL2_HWMON_ATTR : PL1_HWMON_ATTR;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	/*
	 * Max HW supported tau in '(1 + (x / 4)) * power(2,y)' format, x = 0, y = 0x12.
	 * The hwmon->scl_shift_time default of 0xa results in a max tau of 256 seconds.
	 *
	 * The ideal scenario is for PKG_MAX_WIN to be read from the PKG_PWR_SKU register.
	 * However, it is observed that existing discrete GPUs does not provide correct
	 * PKG_MAX_WIN value, therefore a using default constant value. For future discrete GPUs
	 * this may get resolved, in which case PKG_MAX_WIN should be obtained from PKG_PWR_SKU.
	 */
#define PKG_MAX_WIN_DEFAULT 0x12ull

	/*
	 * val must be < max in hwmon interface units. The steps below are
	 * explained in xe_hwmon_power_max_interval_show()
	 */
	r = FIELD_PREP(PKG_MAX_WIN, PKG_MAX_WIN_DEFAULT);
	x = REG_FIELD_GET(PKG_MAX_WIN_X, r);
	y = REG_FIELD_GET(PKG_MAX_WIN_Y, r);
	tau4 = (u64)((1 << x_w) | x) << y;
	max_win = mul_u64_u32_shr(tau4, SF_TIME, hwmon->scl_shift_time + x_w);

	if (val > max_win)
		return -EINVAL;

	if (hwmon->xe->info.platform >= XE_CRESCENTISLAND) {
		/**
		 * On CRI and newer platforms, the interval encoding changed.
		 * The value is now stored directly in milliseconds as U5.2,
		 * replacing the older 1.x * 2^y representation.
		 * Bits [6:2] hold the integer part and bits [1:0] the fraction,so
		 * convert from milliseconds by shifting the value left by 2 to fit into the field.
		 */
		rxy = REG_FIELD_PREP(PWR_LIM_TIME, (val << 2));
	} else {
		/* val in hw units */
		val = DIV_ROUND_CLOSEST_ULL((u64)val << hwmon->scl_shift_time, SF_TIME) + 1;

		/*
		 * Convert val to 1.x * power(2,y)
		 * y = ilog2(val)
		 * x = (val - (1 << y)) >> (y - 2)
		 */
		if (!val) {
			y = 0;
			x = 0;
		} else {
			y = ilog2(val);
			x = (val - (1ul << y)) << x_w >> y;
		}

		rxy = REG_FIELD_PREP(PWR_LIM_TIME_X, x) |
				       REG_FIELD_PREP(PWR_LIM_TIME_Y, y);
	}

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);

	ret = count;
	if (hwmon->xe->info.has_mbx_power_limits) {
		ret = xe_hwmon_pcode_rmw_power_limit(hwmon, power_attr, channel, PWR_LIM_TIME, rxy);
		if (!ret)
			ret = count;
	} else {
		r = xe_mmio_rmw32(mmio, xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel),
				  PWR_LIM_TIME, rxy);
	}

	mutex_unlock(&hwmon->hwmon_lock);

	return ret;
}

/* PSYS PL1 */
static SENSOR_DEVICE_ATTR(power1_max_interval, 0664,
			  xe_hwmon_power_max_interval_show,
			  xe_hwmon_power_max_interval_store, SENSOR_INDEX_PSYS_PL1);
/* PKG PL1 */
static SENSOR_DEVICE_ATTR(power2_max_interval, 0664,
			  xe_hwmon_power_max_interval_show,
			  xe_hwmon_power_max_interval_store, SENSOR_INDEX_PKG_PL1);
/* PSYS PL2 */
static SENSOR_DEVICE_ATTR(power1_cap_interval, 0664,
			  xe_hwmon_power_max_interval_show,
			  xe_hwmon_power_max_interval_store, SENSOR_INDEX_PSYS_PL2);
/* PKG PL2 */
static SENSOR_DEVICE_ATTR(power2_cap_interval, 0664,
			  xe_hwmon_power_max_interval_show,
			  xe_hwmon_power_max_interval_store, SENSOR_INDEX_PKG_PL2);

static struct attribute *hwmon_attributes[] = {
	&sensor_dev_attr_power1_max_interval.dev_attr.attr,
	&sensor_dev_attr_power2_max_interval.dev_attr.attr,
	&sensor_dev_attr_power1_cap_interval.dev_attr.attr,
	&sensor_dev_attr_power2_cap_interval.dev_attr.attr,
	NULL
};

static umode_t xe_hwmon_attributes_visible(struct kobject *kobj,
					   struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	int ret = 0;
	int channel = (index % 2) ? CHANNEL_PKG : CHANNEL_CARD;
	u32 power_attr = (index > 1) ? PL2_HWMON_ATTR : PL1_HWMON_ATTR;
	u32 uval = 0;
	struct xe_reg rapl_limit;
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);

	if (hwmon->xe->info.has_mbx_power_limits) {
		xe_hwmon_pcode_read_power_limit(hwmon, power_attr, channel, &uval);
	} else if (power_attr != PL2_HWMON_ATTR) {
		rapl_limit = xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel);
		if (xe_reg_is_valid(rapl_limit))
			uval = xe_mmio_read32(mmio, rapl_limit);
	}
	ret = (uval & PWR_LIM_EN) ? attr->mode : 0;

	return ret;
}

static const struct attribute_group hwmon_attrgroup = {
	.attrs = hwmon_attributes,
	.is_visible = xe_hwmon_attributes_visible,
};

static const u32 hwmon_temp_config[CHANNEL_MAX + 1] = {
	[CHANNEL_CARD] = HWMON_T_LABEL,
	[CHANNEL_PKG] = HWMON_T_CRIT | HWMON_T_EMERGENCY | HWMON_T_INPUT |
			 HWMON_T_LABEL | HWMON_T_MAX,
	[CHANNEL_VRAM ... CHANNEL_PCIE] = HWMON_T_CRIT | HWMON_T_EMERGENCY |
					 HWMON_T_INPUT | HWMON_T_LABEL,
	[CHANNEL_VRAM_N ... CHANNEL_VRAM_N_MAX] = HWMON_T_CRIT | HWMON_T_EMERGENCY |
							 HWMON_T_INPUT | HWMON_T_LABEL,
	[CHANNEL_MAX] = 0, /* hwmon config terminator */
};

static const struct hwmon_channel_info hwmon_temp_info = {
	.type = hwmon_temp,
	.config = hwmon_temp_config,
};

static const struct hwmon_channel_info * const hwmon_info[] = {
	&hwmon_temp_info,
	HWMON_CHANNEL_INFO(power, HWMON_P_MAX | HWMON_P_RATED_MAX | HWMON_P_LABEL | HWMON_P_CRIT |
			   HWMON_P_CAP,
			   HWMON_P_MAX | HWMON_P_RATED_MAX | HWMON_P_LABEL | HWMON_P_CAP),
	HWMON_CHANNEL_INFO(curr, HWMON_C_LABEL, HWMON_C_CRIT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(energy, HWMON_E_INPUT | HWMON_E_LABEL, HWMON_E_INPUT | HWMON_E_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_MAX, HWMON_F_INPUT | HWMON_F_MAX,
			   HWMON_F_INPUT | HWMON_F_MAX),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static int xe_hwmon_pcode_read_thermal_info(struct xe_hwmon *hwmon)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u32 config = 0, data0 = 0, data1 = 0;
	int ret;

	/*
	 * Only CRI reports dynamic VRAM channel state. Fixed-channel platforms
	 * set the count up front so those sensors stay visible even if the thermal
	 * mailbox reads below fail.
	 */
	if (hwmon->xe->info.has_fixed_vram_channels)
		hwmon->temp.vram_count = FIXED_VRAM_CHANNELS;

	if (hwmon->xe->info.platform == XE_CRESCENTISLAND) {
		hwmon->temp.msu_mask = xe_mmio_read32(mmio, CRI_MSU_VRAM_ENABLE);
		hwmon->temp.vram_count = fls(hwmon->temp.msu_mask) * VRAM_CHANNELS_PER_MSU;
		drm_dbg(&hwmon->xe->drm, "MSU VRAM enable mask 0x%x, VRAM channel bound %d\n",
			hwmon->temp.msu_mask, hwmon->temp.vram_count);
		if (hwmon->temp.vram_count > MAX_VRAM_CHANNELS) {
			drm_warn(&hwmon->xe->drm,
				 "VRAM channel bound %d exceeds max %d, clamping\n",
				 hwmon->temp.vram_count, MAX_VRAM_CHANNELS);
			hwmon->temp.vram_count = MAX_VRAM_CHANNELS;
		}
	}

	ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_THERMAL_INFO, READ_THERMAL_LIMITS, 0),
			    &data0, &data1);
	if (ret)
		return ret;

	drm_dbg(&hwmon->xe->drm, "thermal info read val 0x%x val1 0x%x\n", data0, data1);
	hwmon->temp.data[0] = cpu_to_le32(data0);
	hwmon->temp.data[1] = cpu_to_le32(data1);

	ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_THERMAL_INFO, READ_THERMAL_CONFIG, 0),
			    &config, NULL);
	if (ret)
		return ret;

	drm_dbg(&hwmon->xe->drm, "thermal config count 0x%x\n", config);
	hwmon->temp.count = REG_FIELD_GET(TEMP_MASK, config);

	return ret;
}

static inline bool mmio_temp_valid(const struct xe_hwmon *hwmon, u32 value)
{
	/*
	 * On platforms lacking temperature sentinel support, a value of 0 is treated
	 * as invalid. While 0°C is theoretically possible, it is highly unlikely for
	 * a discrete GPU in normal environments, making it a reasonable fallback
	 * indicator for invalid or uninitialized temperature readings.
	 * All-ones is returned when the device is off the bus, so reject it too.
	 */
	if (value == U32_MAX)
		return false;

	return hwmon->xe->info.has_temp_sentinel || value != 0;
}

/* Mailbox temperature is sign-magnitude (bit 7 sign, bits 6:0 magnitude). */
static inline bool mbx_temp_valid(const struct xe_hwmon *hwmon, u8 value)
{
	if (value == TEMP_MBX_INVALID)
		return false;

	return hwmon->xe->info.has_temp_sentinel || value != 0;
}

/* Decode an 8-bit sign-magnitude mailbox temperature to degrees Celsius. */
static int mbx_temp_decode(u8 value)
{
	int temp = value & TEMP_MBX_MAG;

	return (value & TEMP_MBX_SIGN) ? -temp : temp;
}

static u8 xe_hwmon_pcie_temp_index(const struct xe_hwmon *hwmon)
{
	if (hwmon->xe->info.platform == XE_CRESCENTISLAND)
		return CRI_TEMP_INDEX_PCIE;

	if (hwmon->xe->info.subplatform == XE_SUBPLATFORM_BATTLEMAGE_G21)
		return BMG_G21_TEMP_INDEX_PCIE;

	return TEMP_INDEX_PCIE;
}

static int get_mc_temp(struct xe_hwmon *hwmon, long *val)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	int ret, i, start, end, count = 0;
	s32 average = 0;
	u32 data = 0;

	guard(mutex)(&hwmon->hwmon_lock);

	/* CRI relocated the memory controller temperatures to READ_THERMAL_DATA bytes 16..19. */
	if (hwmon->xe->info.platform == XE_CRESCENTISLAND) {
		start = CRI_TEMP_INDEX_MCTRL;
		end = CRI_TEMP_INDEX_MCTRL + CRI_TEMP_MCTRL_COUNT;
	} else {
		start = TEMP_INDEX_MCTRL;
		end = min_t(int, hwmon->temp.count - 1, xe_hwmon_pcie_temp_index(hwmon));
	}

	if (end <= start || end > hwmon->temp.count) {
		drm_warn(&hwmon->xe->drm,
			 "memory controller range [%d, %d) is invalid for temperature count %d\n",
			 start, end, hwmon->temp.count);
		return -ENODATA;
	}

	for (i = start / sizeof(u32); i < DIV_ROUND_UP(end, sizeof(u32)); i++) {
		ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_THERMAL_INFO, READ_THERMAL_DATA, i),
				    &data, NULL);
		if (ret)
			return ret;
		drm_dbg(&hwmon->xe->drm, "thermal data for group %d val 0x%x\n", i, data);
		hwmon->temp.dword[i] = cpu_to_le32(data);
	}

	for (i = start; i < end; i++) {
		if (!mbx_temp_valid(hwmon, hwmon->temp.value[i]))
			continue;
		average += mbx_temp_decode(hwmon->temp.value[i]);
		count++;
	}

	if (!count) {
		drm_dbg(&hwmon->xe->drm, "Memory controller temperature unavailable!\n");
		return -ENXIO;
	}

	average /= count;

	if (val)
		*val = average * MILLIDEGREE_PER_DEGREE;

	return 0;
}

static int get_pcie_temp(const struct xe_hwmon *hwmon, long *val)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	u32 data = 0;
	int index;
	int ret;

	index = xe_hwmon_pcie_temp_index(hwmon);
	if (index >= hwmon->temp.count) {
		drm_warn(&hwmon->xe->drm,
			 "PCIe temperature index %d out of range for temperature count %d\n",
			 index, hwmon->temp.count);
		return -ENODATA;
	}

	ret = xe_pcode_read(root_tile, PCODE_MBOX(PCODE_THERMAL_INFO, READ_THERMAL_DATA,
						  index / sizeof(data)),
						  &data, NULL);
	if (ret)
		return ret;

	data >>= (index % sizeof(data)) * BITS_PER_BYTE;
	data = REG_FIELD_GET(TEMP_MASK, data);
	if (!mbx_temp_valid(hwmon, data)) {
		drm_dbg(&hwmon->xe->drm, "PCIe temperature not available\n");
		return -ENXIO;
	}

	if (val)
		*val = mbx_temp_decode(data) * MILLIDEGREE_PER_DEGREE;

	return 0;
}

/* I1 is exposed as power_crit or as curr_crit depending on bit 31 */
static int xe_hwmon_pcode_read_i1(const struct xe_hwmon *hwmon, u32 *uval)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);

	/* Avoid Illegal Subcommand error */
	if (hwmon->xe->info.platform == XE_DG2)
		return -ENXIO;

	return xe_pcode_read(root_tile, PCODE_MBOX(PCODE_POWER_SETUP,
			     POWER_SETUP_SUBCOMMAND_READ_I1, 0),
			     uval, NULL);
}

static int xe_hwmon_pcode_write_i1(const struct xe_hwmon *hwmon, u32 uval)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);

	return xe_pcode_write(root_tile, PCODE_MBOX(PCODE_POWER_SETUP,
			      POWER_SETUP_SUBCOMMAND_WRITE_I1, 0),
			      (uval & POWER_SETUP_I1_DATA_MASK));
}

static int xe_hwmon_pcode_read_fan_control(const struct xe_hwmon *hwmon, u32 subcmd, u8 fan_num,
					   u32 *uval)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);

	return xe_pcode_read(root_tile, PCODE_MBOX(FAN_SPEED_CONTROL, subcmd, fan_num),
				     uval, NULL);
}

static int xe_hwmon_get_fan_enable_mask(struct xe_hwmon *hwmon)
{
	int ret;

	/* Platforms that don't return correct value */
	if (hwmon->xe->info.platform == XE_DG2) {
		hwmon->fan_enable_mask = GENMASK(1, 0);
		return 0;
	}

	ret = xe_hwmon_pcode_read_fan_control(hwmon, FSC_READ_NUM_FANS, 0,
					      &hwmon->fan_enable_mask);
	if (ret) {
		hwmon->fan_enable_mask = 0;
		xe_warn(hwmon->xe, "failed to read number of fans, ret=%d\n", ret);
		return ret;
	}

	/* Check if fan mask exceeds FAN_MAX and log a warning if it does */
	if (hwmon->fan_enable_mask & ~GENMASK(FAN_MAX - 1, 0)) {
		xe_warn(hwmon->xe, "Fan enable mask exceeds %d supported fans, clamping\n",
			FAN_MAX);
		hwmon->fan_enable_mask &= GENMASK(FAN_MAX - 1, 0);
	}

	xe_info(hwmon->xe, "Number of fans detected: %u\n", hweight32(hwmon->fan_enable_mask));

	return 0;
}

static int xe_hwmon_get_fan_point_count(struct xe_hwmon *hwmon, u8 fan, u32 *point_count,
					enum fan_table_type table_type)
{
	int ret;

	ret = xe_hwmon_pcode_read_fan_control(hwmon,
					      (table_type == USER_FAN_TABLE) ?
					      FSC_READ_USER_FAN_CONTROL_POINTS :
					      FSC_READ_STOCK_FAN_CONTROL_POINTS,
					      fan, point_count);
	if (ret) {
		*point_count = 0;
		xe_err(hwmon->xe, "failed to read fan %d %s point count, ret=%d\n", fan,
		       (table_type == USER_FAN_TABLE) ? "user" : "stock", ret);
		return ret;
	}

	if (*point_count > MAX_FAN_CONTROL_POINTS) {
		xe_err(hwmon->xe, "fan %d %s point count %u exceeds max %u\n", fan,
		       (table_type == USER_FAN_TABLE) ? "user" : "stock", *point_count,
		       MAX_FAN_CONTROL_POINTS);
		return -EINVAL;
	}

	return 0;
}

static int xe_hwmon_commit_user_fan_table(struct xe_hwmon *hwmon, u8 fan,
					  const struct fan_control_point *points, u32 point_count)
{
	/* The extra write is for the final write that sets the number of fan control points */
	struct xe_pcode_write writes[MAX_FAN_CONTROL_POINTS + 1];
	u32 accepted_count = 0;
	u32 point;
	int ret;

	if (!point_count) {
		xe_err(hwmon->xe, "no fan control points provided for fan %d\n", fan);
		return -ENODATA;
	}

	for (point = 0; point < point_count; point++) {
		u8 temp = points[point].temp;
		u8 speed = points[point].speed;

		/* Temperature and speed must not decrease from one point to the next. */
		if (point > 0) {
			u8 prev_temp = points[point - 1].temp;
			u8 prev_speed = points[point - 1].speed;

			if (temp < prev_temp || speed < prev_speed) {
				xe_dbg(hwmon->xe,
				       "fan %d point %u decreases: %u/%u%% after %u/%u%%\n",
				       fan, point, temp, speed, prev_temp, prev_speed);
				return -EINVAL;
			}
		}

		/* Prepare the write for this fan control point */
		writes[point].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_FAN_TABLE,
						fan);
		writes[point].data = FIELD_PREP(FAN_CONTROL_POINT_TEMP_MASK, temp) |
				FIELD_PREP(FAN_CONTROL_POINT_SPEED_MASK, speed);
	}

	xe_dbg(hwmon->xe, "committing fan %d user table with %u points\n", fan, point_count);

	writes[point_count].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_NUM_FAN_CONTROL_POINTS,
					      fan);
	writes[point_count].data = point_count;

	ret = xe_pcode_write_batch(xe_device_get_root_tile(hwmon->xe), writes, point_count + 1);
	if (ret) {
		xe_dbg(hwmon->xe,
		       "failed to write fan %d user table count to %u, ret=%d\n", fan, point_count,
		       ret);
		return ret;
	}

	/* Verify if the user fan table was accepted correctly */
	ret = xe_hwmon_get_fan_point_count(hwmon, fan, &accepted_count, USER_FAN_TABLE);
	if (ret)
		return ret;

	if (accepted_count != point_count) {
		xe_err(hwmon->xe, "fan %d table count mismatch after commit, expected %u, got %u\n",
		       fan, point_count, accepted_count);
		return -EIO;
	}

	xe_dbg(hwmon->xe, "fan %d user table committed successfully with %u points\n", fan,
	       point_count);
	return 0;
}

static int xe_hwmon_activate_user_fan_table(struct xe_hwmon *hwmon, u8 fan,
					    bool is_full_speed, enum fan_table_type table_source)
{
	struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
	struct xe_pcode_write writes[MAX_FAN_CONTROL_POINTS + 1];
	u32 point_count;
	int ret;
	int point;
	struct fan_table *source_table = &fi->fan_table[table_source];

	xe_dbg(hwmon->xe, "activating fan %d user table from %s table\n", fan,
	       (table_source == USER_FAN_TABLE) ? "user" : "stock");

	point_count = source_table->point_count;
	if (!point_count || point_count > MAX_FAN_CONTROL_POINTS) {
		xe_err(hwmon->xe,
		       "fan %d %s table point count %d is invalid, cannot activate user table\n",
		       fan, (table_source == USER_FAN_TABLE) ? "user" : "stock", point_count);
		return -EINVAL;
	}

	for (point = 0; point < point_count; point++) {
		u8 temp = source_table->fcp[point].temp;
		u8 speed = is_full_speed ? 100 : max_t(u8, source_table->fcp[point].speed,
					    min_t(u32, fi->min_pwm, 100));

		writes[point].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_FAN_TABLE, fan);
		writes[point].data = FIELD_PREP(FAN_CONTROL_POINT_TEMP_MASK, temp) |
					FIELD_PREP(FAN_CONTROL_POINT_SPEED_MASK, speed);
	}

	writes[point_count].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_NUM_FAN_CONTROL_POINTS,
					      fan);
	writes[point_count].data = point_count;
	ret = xe_pcode_write_batch(xe_device_get_root_tile(hwmon->xe), writes, point_count + 1);
	if (ret) {
		xe_dbg(hwmon->xe, "failed to write fan %d user table, ret=%d\n", fan, ret);
		return ret;
	}

	/* Verify if all the user points are set */
	ret = xe_hwmon_get_fan_point_count(hwmon, fan, &point_count, USER_FAN_TABLE);
	if (ret) {
		xe_err(hwmon->xe,
		       "failed to read fan %d user table point count, ret=%d\n", fan, ret);
		return ret;
	}
	if (point_count != source_table->point_count) {
		xe_err(hwmon->xe, "fan %d user table point count mismatch, expected %u, got %u\n",
		       fan, source_table->point_count, point_count);
		return -EIO;
	}

	if (table_source == STOCK_FAN_TABLE) {
		for (point = 0; point < point_count; point++) {
			fi->fan_table[USER_FAN_TABLE].fcp[point].temp =
				source_table->fcp[point].temp;
			fi->fan_table[USER_FAN_TABLE].fcp[point].speed =
				is_full_speed ? 100 : max_t(u8, source_table->fcp[point].speed,
							    min_t(u32, fi->min_pwm, 100));
		}
	}

	fi->fan_table[USER_FAN_TABLE].point_count = point_count;
	fi->fan_table[USER_FAN_TABLE].temp_staged = 0;
	fi->fan_table[USER_FAN_TABLE].speed_staged = 0;
	fi->pwm_enable_mode = XE_FAN_PWM_MANUAL_USER_TABLE;
	fi->is_full_speed = is_full_speed;

	return 0;
}

/* Temperature to set when setting the fan PWM with pwmN. */
#define FIXED_PWM_TEMP	1

static int xe_hwmon_set_user_fan_pwm(struct xe_hwmon *hwmon, u8 fan, u8 pwm, bool full_speed)
{
	struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
	struct xe_pcode_write writes[MIN_FAN_CONTROL_POINTS + 1];
	u32 point_count = 0;
	u8 speed;
	int ret, i;

	if (!full_speed && fi->pwm_enable_mode != XE_FAN_PWM_MANUAL_USER_TABLE) {
		xe_dbg(hwmon->xe, "fan %d manual mode is not active, cannot set pwm\n", fan);
		return -EINVAL;
	}

	/* Fan table speed and min_pwm are in percent. */
	speed = DIV_ROUND_CLOSEST(pwm * 100, U8_MAX);
	speed = max_t(u32, speed, min_t(u32, fi->min_pwm, 100));

	/* Fan control needs at least two points; program them all at the same speed. */
	for (i = 0; i < MIN_FAN_CONTROL_POINTS; i++) {
		writes[i].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_FAN_TABLE, fan);
		writes[i].data = FIELD_PREP(FAN_CONTROL_POINT_TEMP_MASK, FIXED_PWM_TEMP + i) |
				 FIELD_PREP(FAN_CONTROL_POINT_SPEED_MASK, speed);
		xe_dbg(hwmon->xe, "prepared fan control point %d with temp=%d, speed=%u%%\n", i,
		       FIXED_PWM_TEMP + i, speed);
	}

	/* pcode fills the user table from point 0 in write order; the count write commits it. */
	writes[i].mbox = PCODE_MBOX(FAN_SPEED_CONTROL, FSC_WRITE_NUM_FAN_CONTROL_POINTS, fan);
	writes[i].data = MIN_FAN_CONTROL_POINTS;
	ret = xe_pcode_write_batch(xe_device_get_root_tile(hwmon->xe), writes, ARRAY_SIZE(writes));
	if (ret) {
		/* pcode may hold a partial table now, so drop the stale cached value. */
		fi->fan_table[USER_FAN_TABLE].point_count = 0;
		xe_err(hwmon->xe, "failed to write pwm %u (%u%%) for fan %d, ret=%d\n",
		       pwm, speed, fan, ret);
		return ret;
	}
	xe_dbg(hwmon->xe, "applied pwm %u (%u%%) for fan %d\n", pwm, speed, fan);

	ret = xe_hwmon_get_fan_point_count(hwmon, fan, &point_count, USER_FAN_TABLE);
	if (ret || point_count != MIN_FAN_CONTROL_POINTS) {
		fi->fan_table[USER_FAN_TABLE].point_count = 0;
		xe_err(hwmon->xe, "pwm set verify failed for fan %d, ret=%d, point count=%u\n", fan,
		       ret, point_count);
		return ret ?: -EIO;
	}

	/* Update the user fan table cache. */
	for (i = 0; i < MIN_FAN_CONTROL_POINTS; i++) {
		fi->fan_table[USER_FAN_TABLE].fcp[i].temp = FIXED_PWM_TEMP + i;
		fi->fan_table[USER_FAN_TABLE].fcp[i].speed = speed;
		xe_dbg(hwmon->xe, "fan %d user table cache point %d with temp=%d, speed=%u%%\n",
		       fan, i, fi->fan_table[USER_FAN_TABLE].fcp[i].temp,
		       fi->fan_table[USER_FAN_TABLE].fcp[i].speed);
	}
	fi->fan_table[USER_FAN_TABLE].point_count = MIN_FAN_CONTROL_POINTS;
	fi->fan_table[USER_FAN_TABLE].temp_staged = 0;
	fi->fan_table[USER_FAN_TABLE].speed_staged = 0;

	fi->pwm_enable_mode = XE_FAN_PWM_MANUAL_USER_TABLE;
	fi->is_full_speed = full_speed;
	return 0;
}

#define XE_CURVE_IDX(fan, point)	(((fan) << 8) | (point))
#define XE_CURVE_FAN(idx)		((u8)(((idx) >> 8) & 0xff))
#define XE_CURVE_POINT(idx)		((u8)((idx) & 0xff))

static int
xe_hwmon_decode_curve_index(struct xe_hwmon *hwmon, u32 index, u8 *fan, u8 *point)
{
	u8 local_fan = XE_CURVE_FAN(index);
	u8 local_point = XE_CURVE_POINT(index);

	if (local_fan >= FAN_MAX || local_point >= MAX_FAN_CONTROL_POINTS) {
		xe_dbg(hwmon->xe, "invalid fan curve index 0x%x: fan %u point %u\n",
		       index, local_fan, local_point);
		return -EINVAL;
	}

	*fan = local_fan;
	*point = local_point;

	return 0;
}

static enum fan_table_type xe_hwmon_get_active_fan_table(struct xe_hwmon *hwmon, u8 fan)
{
	if (hwmon->fi[fan].pwm_enable_mode == XE_FAN_PWM_AUTO_STOCK_TABLE)
		return STOCK_FAN_TABLE;
	else
		return USER_FAN_TABLE;
}

static ssize_t
xe_hwmon_pwm_auto_point_temp_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	enum fan_table_type active_table;
	struct xe_hwmon_fan_info *fi;
	u32 point_count;
	u8 point;
	u8 temp;
	int ret;
	u8 fan;

	ret = xe_hwmon_decode_curve_index(hwmon, sattr->index, &fan, &point);
	if (ret)
		return ret;

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);

	fi = &hwmon->fi[fan];
	active_table = xe_hwmon_get_active_fan_table(hwmon, fan);

	ret = xe_hwmon_get_fan_point_count(hwmon, fan, &point_count, active_table);
	if (ret)
		goto unlock;

	/* Slots beyond the active curve can still be staged for the next curve. */
	if (point >= point_count &&
	    (active_table != USER_FAN_TABLE ||
	    !(fi->fan_table[USER_FAN_TABLE].temp_staged & BIT(point))))
		temp = 0;
	else if (point >= point_count)
		temp = fi->fan_table[USER_FAN_TABLE].staged_fcp[point].temp;
	else
		temp = fi->fan_table[active_table].fcp[point].temp;
	ret = sysfs_emit(buf, "%u\n", temp * MILLIDEGREE_PER_DEGREE);

unlock:
	mutex_unlock(&hwmon->hwmon_lock);

	return ret;
}

static int xe_hwmon_update_user_fan_point(struct xe_hwmon *hwmon, u8 fan, u32 point,
					  bool is_temp, u8 value)
{
	struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
	struct fan_table *table;
	u32 point_count;
	int ret;

	table = &fi->fan_table[USER_FAN_TABLE];

	if (is_temp) {
		table->staged_fcp[point].temp = value;
		table->temp_staged |= BIT(point);
	} else {
		table->staged_fcp[point].speed = value;
		table->speed_staged |= BIT(point);
	}

	/* Wait for a complete prefix; (0, 0) ends the curve without becoming a point. */
	for (point_count = 0; point_count < MAX_FAN_CONTROL_POINTS; point_count++) {
		if (!(table->temp_staged & BIT(point_count)) ||
		    !(table->speed_staged & BIT(point_count)))
			return 0;
		if (!table->staged_fcp[point_count].temp &&
		    !table->staged_fcp[point_count].speed)
			break;
	}

	if (point_count < MIN_FAN_CONTROL_POINTS) {
		xe_dbg(hwmon->xe, "fan %d curve needs at least %d points, got %u\n",
		       fan, MIN_FAN_CONTROL_POINTS, point_count);
		return -EINVAL;
	}

	ret = xe_hwmon_commit_user_fan_table(hwmon, fan, table->staged_fcp, point_count);
	if (ret)
		return ret;

	/* Publish the new curve only after firmware accepts its point count. */
	memcpy(table->fcp, table->staged_fcp, point_count * sizeof(table->fcp[0]));
	table->point_count = point_count;
	table->temp_staged = 0;
	table->speed_staged = 0;
	fi->pwm_enable_mode = XE_FAN_PWM_MANUAL_USER_TABLE;
	fi->is_full_speed = false;
	return 0;
}

static int xe_hwmon_sysfs_to_hw_val(struct xe_hwmon *hwmon, long val, bool is_temp, u8 *out)
{
	long max = is_temp ? U8_MAX : 100;
	long hw_val;

	/* Temperature is in degrees Celsius, speed in percent. */
	if (is_temp)
		hw_val = DIV_ROUND_CLOSEST(val, MILLIDEGREE_PER_DEGREE);
	else
		hw_val = DIV_ROUND_CLOSEST(val * 100, U8_MAX);

	if (hw_val < 0 || hw_val > max) {
		xe_dbg(hwmon->xe, "%s value %ld out of range\n", is_temp ? "temp" : "pwm", val);
		return -EINVAL;
	}

	*out = hw_val;

	return 0;
}

static ssize_t
xe_hwmon_pwm_auto_point_temp_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	struct xe_hwmon_fan_info *fi;
	long val;
	u8 point;
	u8 temp;
	int ret;
	u8 fan;

	ret = xe_hwmon_decode_curve_index(hwmon, sattr->index, &fan, &point);
	if (ret)
		return ret;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	ret = xe_hwmon_sysfs_to_hw_val(hwmon, val, true, &temp);
	if (ret)
		return ret;

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);
	fi = &hwmon->fi[fan];

	if (fi->pwm_enable_mode != XE_FAN_PWM_MANUAL_USER_TABLE) {
		xe_err(hwmon->xe, "pwm%d_enable not in manual mode; cannot update temp point %d\n",
		       fan, point);
		ret = -EINVAL;
		goto unlock;
	}

	xe_dbg(hwmon->xe, "staging fan %d point %d temp to %u\n", fan, point, temp);
	ret = xe_hwmon_update_user_fan_point(hwmon, fan, point, true, temp);

unlock:
	mutex_unlock(&hwmon->hwmon_lock);

	return ret ? ret : count;
}

static ssize_t
xe_hwmon_pwm_auto_point_pwm_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	enum fan_table_type active_table;
	struct xe_hwmon_fan_info *fi;
	u32 point_count;
	u8 point;
	u8 speed;
	int ret;
	u8 fan;

	ret = xe_hwmon_decode_curve_index(hwmon, sattr->index, &fan, &point);
	if (ret)
		return ret;

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);

	fi = &hwmon->fi[fan];
	active_table = xe_hwmon_get_active_fan_table(hwmon, fan);

	ret = xe_hwmon_get_fan_point_count(hwmon, fan, &point_count, active_table);
	if (ret)
		goto unlock;

	/* Slots beyond the active curve can still be staged for the next curve. */
	if (point >= point_count &&
	    (active_table != USER_FAN_TABLE ||
	    !(fi->fan_table[USER_FAN_TABLE].speed_staged & BIT(point))))
		speed = 0;
	else if (point >= point_count)
		speed = fi->fan_table[USER_FAN_TABLE].staged_fcp[point].speed;
	else
		speed = fi->fan_table[active_table].fcp[point].speed;
	ret = sysfs_emit(buf, "%u\n", DIV_ROUND_CLOSEST(speed * U8_MAX, 100));

unlock:
	mutex_unlock(&hwmon->hwmon_lock);

	return ret;
}

static ssize_t
xe_hwmon_pwm_auto_point_pwm_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	struct xe_hwmon_fan_info *fi;
	long val;
	u8 point;
	u8 speed;
	int ret;
	u8 fan;

	ret = xe_hwmon_decode_curve_index(hwmon, sattr->index, &fan, &point);
	if (ret)
		return ret;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	ret = xe_hwmon_sysfs_to_hw_val(hwmon, val, false, &speed);
	if (ret)
		return ret;

	guard(xe_pm_runtime)(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);
	fi = &hwmon->fi[fan];

	if (fi->pwm_enable_mode != XE_FAN_PWM_MANUAL_USER_TABLE) {
		xe_err(hwmon->xe,
		       "pwm%d_enable is not in manual user mode, cannot update pwm point %d\n",
		       fan, point);
		ret = -EINVAL;
		goto unlock;
	}

	/* Speed and min_pwm are in percent; keep zero for the (0, 0) end marker. */
	if (speed)
		speed = max_t(u32, speed, min_t(u32, fi->min_pwm, 100));

	xe_dbg(hwmon->xe, "staging fan %d point %d speed to %u%%\n", fan, point, speed);
	ret = xe_hwmon_update_user_fan_point(hwmon, fan, point, false, speed);

unlock:
	mutex_unlock(&hwmon->hwmon_lock);

	return ret ? ret : count;
}

static umode_t xe_hwmon_curve_attributes_visible(struct kobject *kobj,
						 struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);
	u32 fan = index / XE_CURVE_ATTRS_PER_FAN;

	if (!hwmon->xe->info.has_fan_control || !IS_FAN_ENABLED(hwmon, fan))
		return 0;

	/* All slots must exist at registration, before the user curve has been staged. */
	return attr->mode;
}

static void xe_hwmon_init_curve_attrs(struct xe_hwmon *hwmon)
{
	int fan, kind, point, i = 0;

	/* Per fan: all temp points then all pwm points; is_visible relies on this layout. */
	for (fan = 0; fan < FAN_MAX; fan++) {
		for (kind = 0; kind < 2; kind++) {
			bool is_temp = !kind;

			for (point = 0; point < MAX_FAN_CONTROL_POINTS; point++, i++) {
				struct sensor_device_attribute *sda = &hwmon->curve_sda[i];

				snprintf(hwmon->curve_names[i], XE_CURVE_ATTR_NAME_LEN,
					 "pwm%d_auto_point%d_%s", fan + 1, point + 1,
					 is_temp ? "temp" : "pwm");
				sysfs_attr_init(&sda->dev_attr.attr);
				sda->dev_attr.attr.name = hwmon->curve_names[i];
				sda->dev_attr.attr.mode = 0644;
				if (is_temp) {
					sda->dev_attr.show = xe_hwmon_pwm_auto_point_temp_show;
					sda->dev_attr.store = xe_hwmon_pwm_auto_point_temp_store;
				} else {
					sda->dev_attr.show = xe_hwmon_pwm_auto_point_pwm_show;
					sda->dev_attr.store = xe_hwmon_pwm_auto_point_pwm_store;
				}
				sda->index = XE_CURVE_IDX(fan, point);
				hwmon->curve_attrs[i] = &sda->dev_attr.attr;
			}
		}
	}

	hwmon->curve_group.attrs = hwmon->curve_attrs;
	hwmon->curve_group.is_visible = xe_hwmon_curve_attributes_visible;
}

static int xe_hwmon_read_fan_control_info(struct xe_hwmon *hwmon, u32 new_fan_mask)
{
	int point;
	int fan;
	int ret;
	int err = 0;

	/* Stock table failures leave fan_input and fan_max visible; they don't need it. */
	for (fan = 0; fan < FAN_MAX; fan++) {
		struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
		u32 stock_fcp_count = 0;

		if (!IS_FAN_ENABLED(hwmon, fan))
			continue;

		ret = xe_hwmon_pcode_read_fan_control(hwmon, FSC_READ_MAX_FAN_RPS, fan,
						      &fi->max_rps);
		if (ret) {
			fi->max_rps = 0;
			xe_warn(hwmon->xe, "failed to read fan %d max RPS, ret=%d\n", fan, ret);
		} else {
			xe_dbg(hwmon->xe, "fan %d max RPS %u\n", fan, fi->max_rps);
		}

		ret = xe_hwmon_pcode_read_fan_control(hwmon,
						      FSC_READ_STOCK_FAN_CONTROL_POINTS,
						      fan, &stock_fcp_count);
		if (ret || !stock_fcp_count) {
			xe_warn(hwmon->xe,
				"fan %d stock table unavailable, ret=%d, point count=%u\n",
				fan, ret, ret ? 0 : stock_fcp_count);
			err = ret ?: -ENODATA;
			continue;
		}

		if (stock_fcp_count > MAX_FAN_CONTROL_POINTS) {
			xe_warn(hwmon->xe,
				"fan %d stock table point count %u exceeds maximum %u, clamping\n",
				fan, stock_fcp_count, MAX_FAN_CONTROL_POINTS);
			stock_fcp_count = MAX_FAN_CONTROL_POINTS;
		}

		fi->fan_table[STOCK_FAN_TABLE].point_count = stock_fcp_count;
		xe_dbg(hwmon->xe, "fan %d stock table point count is %u\n", fan,
		       fi->fan_table[STOCK_FAN_TABLE].point_count);

		/* Cache the stock fan table for later use. */
		for (point = 0; point < fi->fan_table[STOCK_FAN_TABLE].point_count;
		     point++) {
			u32 fcp = FCP_INDEX(STOCK_FAN_TABLE, point);

			ret = xe_hwmon_pcode_read_fan_control(hwmon, FSC_READ_FAN_TABLE, fan, &fcp);
			if (ret) {
				xe_warn(hwmon->xe, "failed to read fan %d stock point %d, ret=%d\n",
					fan, point, ret);
				fi->fan_table[STOCK_FAN_TABLE].point_count = 0;
				err = ret;
				break;
			}

			fi->fan_table[STOCK_FAN_TABLE].fcp[point].temp =
						REG_FIELD_GET(FAN_CONTROL_POINT_TEMP_MASK, fcp);
			fi->fan_table[STOCK_FAN_TABLE].fcp[point].speed =
						REG_FIELD_GET(FAN_CONTROL_POINT_SPEED_MASK, fcp);
			xe_dbg(hwmon->xe, "fan %d stock point %d: temp %u C, speed %u %%\n",
			       fan, point, fi->fan_table[STOCK_FAN_TABLE].fcp[point].temp,
			       fi->fan_table[STOCK_FAN_TABLE].fcp[point].speed);
		}

		if (!fi->fan_table[STOCK_FAN_TABLE].point_count)
			continue;

		/* Start new fans in automatic mode; keep the user's mode for known fans. */
		if (new_fan_mask & BIT(fan))
			fi->pwm_enable_mode = XE_FAN_PWM_AUTO_STOCK_TABLE;

		/* Read minimum fan PWM */
		ret = xe_hwmon_pcode_read_fan_control(hwmon, FSC_READ_FAN_MIN_PWM, fan,
						      &fi->min_pwm);
		if (ret) {
			fi->min_pwm = 0;
			xe_warn(hwmon->xe, "fan %d min pwm unavailable, ret=%d\n", fan, ret);
		} else {
			xe_dbg(hwmon->xe, "fan %d min PWM %u\n", fan, fi->min_pwm);
		}
	}
	return err;
}

static int xe_hwmon_power_curr_crit_read(struct xe_hwmon *hwmon, int channel,
					 long *value, u32 scale_factor)
{
	int ret;
	u32 uval = 0;

	mutex_lock(&hwmon->hwmon_lock);

	ret = xe_hwmon_pcode_read_i1(hwmon, &uval);
	if (ret)
		goto unlock;

	*value = mul_u64_u32_shr(REG_FIELD_GET(POWER_SETUP_I1_DATA_MASK, uval),
				 scale_factor, POWER_SETUP_I1_SHIFT);
unlock:
	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static int xe_hwmon_power_curr_crit_write(struct xe_hwmon *hwmon, int channel,
					  long value, u32 scale_factor)
{
	int ret;
	u32 uval;
	u64 max_crit_power_curr = 0;

	mutex_lock(&hwmon->hwmon_lock);

	/*
	 * If the sysfs value exceeds the pcode mailbox cmd POWER_SETUP_SUBCOMMAND_WRITE_I1
	 * max supported value, clamp it to the command's max (U10.6 format).
	 * This is to avoid truncation during uval calculation below and ensure the valid power
	 * limit is sent for pcode which would clamp it to card-supported value.
	 */
	max_crit_power_curr = (POWER_SETUP_I1_DATA_MASK >> POWER_SETUP_I1_SHIFT) * scale_factor;
	if (value > max_crit_power_curr) {
		value = max_crit_power_curr;
		drm_info(&hwmon->xe->drm,
			 "Power limit clamped as selected exceeds channel %d limit\n",
			 channel);
	}
	uval = DIV_ROUND_CLOSEST_ULL(value << POWER_SETUP_I1_SHIFT, scale_factor);
	ret = xe_hwmon_pcode_write_i1(hwmon, uval);

	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static void xe_hwmon_get_voltage(struct xe_hwmon *hwmon, int channel, long *value)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u64 reg_val;

	reg_val = xe_mmio_read32(mmio, xe_hwmon_get_reg(hwmon, REG_GT_PERF_STATUS, channel));
	/* HW register value in units of 2.5 millivolt */
	*value = DIV_ROUND_CLOSEST(REG_FIELD_GET(VOLTAGE_MASK, reg_val) * 2500, SF_VOLTAGE);
}

static bool xe_hwmon_temp_probe(struct xe_hwmon *hwmon, int channel)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	struct xe_reg reg;
	u32 reg_val;

	switch (channel) {
	case CHANNEL_PKG:
	case CHANNEL_VRAM:
	case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
		reg = xe_hwmon_get_reg(hwmon, REG_TEMP, channel);
		if (!xe_reg_is_valid(reg))
			return false;

		/*
		 * On CRI, VRAM channel presence comes from the MSU enable mask
		 * (applied in xe_hwmon_get_reg()); the temperature register's
		 * 0xffffffff sentinel is not reliable for VRAM channels.
		 */
		if (hwmon->xe->info.platform == XE_CRESCENTISLAND && channel >= CHANNEL_VRAM_N)
			return true;

		reg_val = xe_mmio_read32(mmio, reg);
		if (!mmio_temp_valid(hwmon, reg_val)) {
			drm_dbg(&hwmon->xe->drm,
				"channel %d temperature unavailable, val 0x%x\n", channel, reg_val);
			return false;
		}

		return true;
	case CHANNEL_MCTRL:
		return hwmon->temp.count && !get_mc_temp(hwmon, NULL);
	case CHANNEL_PCIE:
		return hwmon->temp.count && !get_pcie_temp(hwmon, NULL);
	default:
		return false;
	}
}

static int xe_hwmon_init_temp_info(struct xe_hwmon *hwmon)
{
	struct device *dev = hwmon->xe->drm.dev;
	int channel;

	if (hwmon->xe->info.has_mbx_thermal_info && xe_hwmon_pcode_read_thermal_info(hwmon))
		drm_warn(&hwmon->xe->drm, "Thermal mailbox not supported by card firmware\n");

	/* vram_count is known only after reading thermal info above. */
	if (hwmon->temp.vram_count) {
		hwmon->temp.vram_label = devm_kcalloc(dev, hwmon->temp.vram_count,
						      MAX_LABEL_SIZE, GFP_KERNEL);
		if (!hwmon->temp.vram_label)
			return -ENOMEM;
	}

	for (channel = 0; channel < CHANNEL_MAX; channel++) {
		hwmon->temp.available[channel] = xe_hwmon_temp_probe(hwmon, channel);
		if (hwmon->temp.available[channel] && hwmon->temp.vram_label &&
		    in_range(channel, CHANNEL_VRAM_N, hwmon->temp.vram_count))
			snprintf(hwmon->temp.vram_label[channel - CHANNEL_VRAM_N], MAX_LABEL_SIZE,
				 "vram_ch_%d", channel - CHANNEL_VRAM_N);
	}

	return 0;
}

static umode_t
xe_hwmon_temp_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	switch (attr) {
	case hwmon_temp_emergency:
		switch (channel) {
		case CHANNEL_PKG:
		case CHANNEL_MCTRL:
		case CHANNEL_PCIE:
			return (hwmon->temp.available[channel] &&
				hwmon->temp.limit[TEMP_LIMIT_PKG_SHUTDOWN]) ? 0444 : 0;
		case CHANNEL_VRAM:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			return (hwmon->temp.available[channel] &&
				hwmon->temp.limit[TEMP_LIMIT_MEM_SHUTDOWN]) ? 0444 : 0;
		default:
			return 0;
		}
	case hwmon_temp_crit:
		switch (channel) {
		case CHANNEL_PKG:
		case CHANNEL_MCTRL:
		case CHANNEL_PCIE:
			return (hwmon->temp.available[channel] &&
				hwmon->temp.limit[TEMP_LIMIT_PKG_CRIT]) ? 0444 : 0;
		case CHANNEL_VRAM:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			return (hwmon->temp.available[channel] &&
				hwmon->temp.limit[TEMP_LIMIT_MEM_CRIT]) ? 0444 : 0;
		default:
			return 0;
		}
	case hwmon_temp_max:
		switch (channel) {
		case CHANNEL_PKG:
			return (hwmon->temp.available[channel] &&
				hwmon->temp.limit[TEMP_LIMIT_PKG_MAX]) ? 0444 : 0;
		default:
			return 0;
		}
	case hwmon_temp_input:
	case hwmon_temp_label:
		switch (channel) {
		case CHANNEL_PKG:
		case CHANNEL_VRAM:
		case CHANNEL_MCTRL:
		case CHANNEL_PCIE:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			return hwmon->temp.available[channel] ? 0444 : 0;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

#define IEEE754_SIGN		REG_BIT(31)
#define IEEE754_EXP_MASK	REG_GENMASK(30, 23)
#define IEEE754_MANTISSA_MASK	REG_GENMASK(22, 0)
#define IEEE754_EXP_BIAS	127 /* Exponent bias to obtain the actual exponent */
#define IEEE754_MANTISSA_BITS	23 /* Mantissa width and implicit leading-1 bit position */
#define IEEE754_EXP_SPECIAL	0xff /* reserved exponent value encoding infinity or NaN */

/**
 * xe_hwmon_temperature_decode - Decode an IEEE-754 temperature register value
 * @reg_val: The raw register value to decode
 * @temperature: Pointer to store the decoded temperature in millidegrees Celsius.
 *
 * IEEE-754 single-precision format:
 *
 *  31 30                         23 22                         0
 * +--+-----------------------------+----------------------------+
 * | S|          exponent           |          fraction          |
 * +--+-----------------------------+----------------------------+
 *
 * value = (-1)^S * 1.fraction * 2^(exponent - 127)
 *
 * Return: 0 on success or an error if @reg_val is non-finite or cannot be
 * represented in millidegrees Celsius.
 */
static int xe_hwmon_temperature_decode(u32 reg_val, long *temperature)
{
	u32 exponent = REG_FIELD_GET(IEEE754_EXP_MASK, reg_val);
	u32 mantissa = REG_FIELD_GET(IEEE754_MANTISSA_MASK, reg_val);
	s64 value;
	int shift;

	/* Exponent 0xff encodes infinity or NaN. */
	if (exponent == IEEE754_EXP_SPECIAL)
		return -EINVAL;

	/* Subnormal values are smaller than one millidegree Celsius. */
	if (!exponent) {
		*temperature = 0;
		return 0;
	}

	value = (s64)(mantissa | BIT(IEEE754_MANTISSA_BITS)) * MILLIDEGREE_PER_DEGREE;
	shift = exponent - (IEEE754_EXP_BIAS + IEEE754_MANTISSA_BITS);
	if (shift >= 0) {
		if (shift >= BITS_PER_LONG || value > (LONG_MAX >> shift))
			return -ERANGE;
		value <<= shift;
	} else if (shift > -64) {
		value >>= -shift;
	} else {
		value = 0;
	}

	/* Ensure the decoded value fits in a long. */
	if (overflows_type(value, *temperature))
		return -ERANGE;

	if (reg_val & IEEE754_SIGN)
		value = -value;

	*temperature = value;
	return 0;
}

static int
xe_hwmon_temp_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	u64 reg_val;

	switch (attr) {
	case hwmon_temp_input:
		switch (channel) {
		case CHANNEL_MCTRL:
			return get_mc_temp(hwmon, val);
		case CHANNEL_PCIE:
			return get_pcie_temp(hwmon, val);
		case CHANNEL_PKG:
		case CHANNEL_VRAM:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			reg_val = xe_mmio_read32(mmio, xe_hwmon_get_reg(hwmon, REG_TEMP, channel));
			if (!mmio_temp_valid(hwmon, reg_val)) {
				drm_dbg(&hwmon->xe->drm,
					"channel %d temperature invalid, val 0x%llx\n",
					channel, reg_val);
				return -ENXIO;
			}
			if (hwmon->xe->info.platform >= XE_CRESCENTISLAND) {
				return xe_hwmon_temperature_decode((u32)reg_val, val);
			} else if (channel >= CHANNEL_VRAM_N) {
				/*
				 * This temperature format is 24 bit [31:8] signed integer and 8 bit
				 * [7:0] fraction for platforms before CRI.
				 */
				*val = (s32)REG_FIELD_GET(TEMP_MASK_VRAM_N, reg_val) *
					(REG_FIELD_GET(TEMP_SIGN_MASK, reg_val) ? -1 : 1) *
					MILLIDEGREE_PER_DEGREE;
			} else {
				*val = REG_FIELD_GET(TEMP_MASK, reg_val) * MILLIDEGREE_PER_DEGREE;
			}
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_temp_emergency:
		switch (channel) {
		case CHANNEL_PKG:
		case CHANNEL_MCTRL:
		case CHANNEL_PCIE:
			*val = hwmon->temp.limit[TEMP_LIMIT_PKG_SHUTDOWN] * MILLIDEGREE_PER_DEGREE;
			return 0;
		case CHANNEL_VRAM:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			*val = hwmon->temp.limit[TEMP_LIMIT_MEM_SHUTDOWN] * MILLIDEGREE_PER_DEGREE;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_temp_crit:
		switch (channel) {
		case CHANNEL_PKG:
		case CHANNEL_MCTRL:
		case CHANNEL_PCIE:
			*val = hwmon->temp.limit[TEMP_LIMIT_PKG_CRIT] * MILLIDEGREE_PER_DEGREE;
			return 0;
		case CHANNEL_VRAM:
		case CHANNEL_VRAM_N...CHANNEL_VRAM_N_MAX:
			*val = hwmon->temp.limit[TEMP_LIMIT_MEM_CRIT] * MILLIDEGREE_PER_DEGREE;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_temp_max:
		switch (channel) {
		case CHANNEL_PKG:
			*val = hwmon->temp.limit[TEMP_LIMIT_PKG_MAX] * MILLIDEGREE_PER_DEGREE;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
xe_hwmon_power_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	u32 uval = 0;
	struct xe_reg reg;
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);

	switch (attr) {
	case hwmon_power_max:
	case hwmon_power_cap:
		if (hwmon->xe->info.has_mbx_power_limits) {
			xe_hwmon_pcode_read_power_limit(hwmon, attr, channel, &uval);
		} else if (attr != PL2_HWMON_ATTR) {
			reg = xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel);
			if (xe_reg_is_valid(reg))
				uval = xe_mmio_read32(mmio, reg);
		}
		if (uval & PWR_LIM_EN) {
			drm_info(&hwmon->xe->drm, "%s is supported on channel %d\n",
				 PWR_ATTR_TO_STR(attr), channel);
			return 0664;
		}
		drm_dbg(&hwmon->xe->drm, "%s is unsupported on channel %d\n",
			PWR_ATTR_TO_STR(attr), channel);
		return 0;
	case hwmon_power_rated_max:
		if (hwmon->xe->info.has_mbx_power_limits) {
			return 0;
		} else {
			reg = xe_hwmon_get_reg(hwmon, REG_PKG_POWER_SKU, channel);
			if (xe_reg_is_valid(reg))
				uval = xe_mmio_read32(mmio, reg);
			return uval ? 0444 : 0;
		}
	case hwmon_power_crit:
		if (channel == CHANNEL_CARD) {
			if (xe_hwmon_pcode_read_i1(hwmon, &uval))
				return 0;
			return (uval & POWER_SETUP_I1_WATTS) ? 0644 : 0;
		}
		break;
	case hwmon_power_label:
		if (hwmon->xe->info.has_mbx_power_limits) {
			xe_hwmon_pcode_read_power_limit(hwmon, attr, channel, &uval);
		} else {
			reg = xe_hwmon_get_reg(hwmon, REG_PKG_POWER_SKU, channel);
			if (xe_reg_is_valid(reg))
				uval = xe_mmio_read32(mmio, reg);

			if (!uval) {
				reg = xe_hwmon_get_reg(hwmon, REG_PKG_RAPL_LIMIT, channel);
				if (xe_reg_is_valid(reg))
					uval = xe_mmio_read32(mmio, reg);
			}
		}
		if ((!(uval & PWR_LIM_EN)) && channel == CHANNEL_CARD) {
			if (xe_hwmon_pcode_read_i1(hwmon, &uval))
				return 0;
			return (uval & POWER_SETUP_I1_WATTS) ? 0444 : 0;
		}
		return (uval) ? 0444 : 0;
	default:
		return 0;
	}
	return 0;
}

static int
xe_hwmon_power_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_power_max:
	case hwmon_power_cap:
		return xe_hwmon_power_max_read(hwmon, attr, channel, val);
	case hwmon_power_rated_max:
		return xe_hwmon_power_rated_max_read(hwmon, attr, channel, val);
	case hwmon_power_crit:
		return xe_hwmon_power_curr_crit_read(hwmon, channel, val, SF_POWER);
	default:
		return -EOPNOTSUPP;
	}
}

static int
xe_hwmon_power_write(struct xe_hwmon *hwmon, u32 attr, int channel, long val)
{
	switch (attr) {
	case hwmon_power_cap:
	case hwmon_power_max:
		return xe_hwmon_power_max_write(hwmon, attr, channel, val);
	case hwmon_power_crit:
		return xe_hwmon_power_curr_crit_write(hwmon, channel, val, SF_POWER);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
xe_hwmon_curr_is_visible(const struct xe_hwmon *hwmon, u32 attr, int channel)
{
	u32 uval = 0;

	/* hwmon sysfs attribute of current available only for package */
	if (channel != CHANNEL_PKG)
		return 0;

	switch (attr) {
	case hwmon_curr_crit:
			return (xe_hwmon_pcode_read_i1(hwmon, &uval) ||
				(uval & POWER_SETUP_I1_WATTS)) ? 0 : 0644;
	case hwmon_curr_label:
			return (xe_hwmon_pcode_read_i1(hwmon, &uval) ||
				(uval & POWER_SETUP_I1_WATTS)) ? 0 : 0444;
		break;
	default:
		return 0;
	}
	return 0;
}

static int
xe_hwmon_curr_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_curr_crit:
		return xe_hwmon_power_curr_crit_read(hwmon, channel, val, SF_CURR);
	default:
		return -EOPNOTSUPP;
	}
}

static int
xe_hwmon_curr_write(struct xe_hwmon *hwmon, u32 attr, int channel, long val)
{
	switch (attr) {
	case hwmon_curr_crit:
		return xe_hwmon_power_curr_crit_write(hwmon, channel, val, SF_CURR);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
xe_hwmon_in_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	switch (attr) {
	case hwmon_in_input:
	case hwmon_in_label:
		return xe_reg_is_valid(xe_hwmon_get_reg(hwmon, REG_GT_PERF_STATUS,
				       channel)) ? 0444 : 0;
	default:
		return 0;
	}
}

static int
xe_hwmon_in_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_in_input:
		xe_hwmon_get_voltage(hwmon, channel, val);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
xe_hwmon_energy_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	long energy = 0;

	switch (attr) {
	case hwmon_energy_input:
	case hwmon_energy_label:
		if (hwmon->xe->info.platform == XE_BATTLEMAGE) {
			xe_hwmon_energy_get(hwmon, channel, &energy);
			return energy ? 0444 : 0;
		} else {
			return xe_reg_is_valid(xe_hwmon_get_reg(hwmon, REG_PKG_ENERGY_STATUS,
					       channel)) ? 0444 : 0;
		}
	default:
		return 0;
	}
}

static int
xe_hwmon_energy_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_energy_input:
		return xe_hwmon_energy_get(hwmon, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
xe_hwmon_fan_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	if (!hwmon->xe->info.has_fan_control)
		return 0;

	switch (attr) {
	case hwmon_fan_input:
		return IS_FAN_ENABLED(hwmon, channel) ? 0444 : 0;
	case hwmon_fan_max:
		return IS_FAN_ENABLED(hwmon, channel) && hwmon->fi[channel].max_rps ? 0444 : 0;
	default:
		return 0;
	}
}

static umode_t
xe_hwmon_pwm_is_visible(struct xe_hwmon *hwmon, u32 attr, int channel)
{
	if (!hwmon->xe->info.has_fan_control || !IS_FAN_ENABLED(hwmon, channel))
		return 0;

	switch (attr) {
	case hwmon_pwm_input:
	case hwmon_pwm_enable:
		return 0644;
	default:
		return 0;
	}
}

static int
xe_hwmon_fan_input_read(struct xe_hwmon *hwmon, int channel, long *val)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	struct xe_hwmon_fan_info *fi = &hwmon->fi[channel];
	u64 rotations, time_now, time;
	u32 reg_val;
	int ret = 0;

	mutex_lock(&hwmon->hwmon_lock);

	reg_val = xe_mmio_read32(mmio, xe_hwmon_get_reg(hwmon, REG_FAN_SPEED, channel));
	time_now = get_jiffies_64();

	/*
	 * HW register value is accumulated count of pulses from PWM fan with the scale
	 * of 2 pulses per rotation.
	 */
	rotations = (reg_val - fi->reg_val_prev) / 2;

	time = jiffies_delta_to_msecs(time_now - fi->time_prev);
	if (unlikely(!time)) {
		ret = -EAGAIN;
		goto unlock;
	}

	/*
	 * Calculate fan speed in RPM by time averaging two subsequent readings in minutes.
	 * RPM = number of rotations * msecs per minute / time in msecs
	 */
	*val = DIV_ROUND_UP_ULL(rotations * (MSEC_PER_SEC * 60), time);

	fi->reg_val_prev = reg_val;
	fi->time_prev = time_now;
unlock:
	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static int
xe_hwmon_fan_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_fan_input:
		return xe_hwmon_fan_input_read(hwmon, channel, val);
	case hwmon_fan_max:
		*val = mul_u32_u32(hwmon->fi[channel].max_rps, 60);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int
xe_hwmon_pwm_read(struct xe_hwmon *hwmon, u32 attr, int channel, long *val)
{
	struct xe_hwmon_fan_info *fi;
	int ret = 0;

	mutex_lock(&hwmon->hwmon_lock);

	fi = &hwmon->fi[channel];

	switch (attr) {
	case hwmon_pwm_input:
		if (!fi->fan_table[USER_FAN_TABLE].point_count) {
			xe_dbg(hwmon->xe, "pwm not set for fan %d\n", channel);
			ret = -ENODATA;
			goto unlock;
		}
		/* Speed is cached in percent, so the 0..255 round trip may be off by a step. */
		*val = DIV_ROUND_CLOSEST(fi->fan_table[USER_FAN_TABLE].fcp[0].speed * U8_MAX, 100);
		xe_dbg(hwmon->xe, "pwm read for fan %d: %ld\n", channel, *val);
		break;
	case hwmon_pwm_enable:
		*val = fi->is_full_speed ? XE_FAN_PWM_FULL_SPEED : fi->pwm_enable_mode;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

unlock:
	mutex_unlock(&hwmon->hwmon_lock);
	return ret;
}

static int
xe_hwmon_pwm_write(struct xe_hwmon *hwmon, u32 attr, int channel, long val)
{
	struct xe_tile *root_tile = xe_device_get_root_tile(hwmon->xe);
	struct xe_hwmon_fan_info *fi;
	int ret = 0;

	fi = &hwmon->fi[channel];

	mutex_lock(&hwmon->hwmon_lock);

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > U8_MAX) {
			ret = -EINVAL;
			break;
		}
		ret = xe_hwmon_set_user_fan_pwm(hwmon, channel, (u8)val, false);
		break;
	case hwmon_pwm_enable:
		if (val == XE_FAN_PWM_FULL_SPEED) {
			ret = xe_hwmon_set_user_fan_pwm(hwmon, channel, U8_MAX, true);
			if (ret) {
				xe_warn(hwmon->xe,
					"full speed enable for fan %d failed, ret=%d\n",
					channel, ret);
				break;
			}
		} else if (val == XE_FAN_PWM_MANUAL_USER_TABLE) {
			enum fan_table_type source = fi->fan_table[USER_FAN_TABLE].point_count ?
				USER_FAN_TABLE : STOCK_FAN_TABLE;

			/*
			 * Manual fan speed control using user fan table.
			 * If user fan table is not programmed, it would be initialized with
			 * clamped stock points which allows manual control with some
			 * default points.
			 */
			ret = xe_hwmon_activate_user_fan_table(hwmon, channel, false, source);
			if (ret) {
				xe_warn(hwmon->xe,
					"failed to activate user fan table for fan %d, ret=%d\n",
					channel, ret);
				break;
			}
		} else if (val == XE_FAN_PWM_AUTO_STOCK_TABLE) {
			/* Switch to stock table specifying the user points to 0. */
			ret = xe_pcode_write(root_tile, PCODE_MBOX(FAN_SPEED_CONTROL,
								   FSC_WRITE_NUM_FAN_CONTROL_POINTS,
								   channel), 0);
			if (ret) {
				xe_warn(hwmon->xe,
					"failed to disable user fan table for fan %d, ret=%d\n",
					channel, ret);
				break;
			}
			xe_dbg(hwmon->xe,
			       "automatic control enabled for fan %d using stock table\n",
			       channel);
			fi->pwm_enable_mode = XE_FAN_PWM_AUTO_STOCK_TABLE;
			fi->is_full_speed = false;
			fi->fan_table[USER_FAN_TABLE].temp_staged = 0;
			fi->fan_table[USER_FAN_TABLE].speed_staged = 0;
		} else {
			ret = -EINVAL;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&hwmon->hwmon_lock);

	return ret;
}

static umode_t
xe_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type,
		    u32 attr, int channel)
{
	struct xe_hwmon *hwmon = (struct xe_hwmon *)drvdata;
	int ret;

	switch (type) {
	case hwmon_temp:
		ret = xe_hwmon_temp_is_visible(hwmon, attr, channel);
		break;
	case hwmon_power:
		ret = xe_hwmon_power_is_visible(hwmon, attr, channel);
		break;
	case hwmon_curr:
		ret = xe_hwmon_curr_is_visible(hwmon, attr, channel);
		break;
	case hwmon_in:
		ret = xe_hwmon_in_is_visible(hwmon, attr, channel);
		break;
	case hwmon_energy:
		ret = xe_hwmon_energy_is_visible(hwmon, attr, channel);
		break;
	case hwmon_fan:
		ret = xe_hwmon_fan_is_visible(hwmon, attr, channel);
		break;
	case hwmon_pwm:
		ret = xe_hwmon_pwm_is_visible(hwmon, attr, channel);
		break;
	default:
		ret = 0;
		break;
	}

	return ret;
}

static int
xe_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
	      int channel, long *val)
{
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);

	guard(xe_pm_runtime)(hwmon->xe);

	switch (type) {
	case hwmon_temp:
		return xe_hwmon_temp_read(hwmon, attr, channel, val);
	case hwmon_power:
		return xe_hwmon_power_read(hwmon, attr, channel, val);
	case hwmon_curr:
		return xe_hwmon_curr_read(hwmon, attr, channel, val);
	case hwmon_in:
		return xe_hwmon_in_read(hwmon, attr, channel, val);
	case hwmon_energy:
		return xe_hwmon_energy_read(hwmon, attr, channel, val);
	case hwmon_fan:
		return xe_hwmon_fan_read(hwmon, attr, channel, val);
	case hwmon_pwm:
		return xe_hwmon_pwm_read(hwmon, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int
xe_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
	       int channel, long val)
{
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);

	guard(xe_pm_runtime)(hwmon->xe);

	switch (type) {
	case hwmon_power:
		return xe_hwmon_power_write(hwmon, attr, channel, val);
	case hwmon_curr:
		return xe_hwmon_curr_write(hwmon, attr, channel, val);
	case hwmon_pwm:
		return xe_hwmon_pwm_write(hwmon, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int xe_hwmon_read_label(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	struct xe_hwmon *hwmon = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (channel == CHANNEL_PKG)
			*str = "pkg";
		else if (channel == CHANNEL_VRAM)
			*str = "vram";
		else if (channel == CHANNEL_MCTRL)
			*str = "mctrl";
		else if (channel == CHANNEL_PCIE)
			*str = "pcie";
		else if (in_range(channel, CHANNEL_VRAM_N, hwmon->temp.vram_count))
			*str = hwmon->temp.vram_label[channel - CHANNEL_VRAM_N];
		else
			return -EOPNOTSUPP;
		return 0;
	case hwmon_power:
	case hwmon_energy:
	case hwmon_curr:
	case hwmon_in:
		if (channel == CHANNEL_CARD)
			*str = "card";
		else if (channel == CHANNEL_PKG)
			*str = "pkg";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = xe_hwmon_is_visible,
	.read = xe_hwmon_read,
	.write = xe_hwmon_write,
	.read_string = xe_hwmon_read_label,
};

static const struct hwmon_chip_info hwmon_chip_info = {
	.ops = &hwmon_ops,
	.info = hwmon_info,
};

static int
xe_hwmon_get_preregistration_info(struct xe_hwmon *hwmon)
{
	struct xe_mmio *mmio = xe_root_tile_mmio(hwmon->xe);
	long energy, fan_speed;
	u64 val_sku_unit = 0;
	int channel;
	struct xe_reg pkg_power_sku_unit;

	if (hwmon->xe->info.has_mbx_power_limits) {
		/* Check if GPU firmware support mailbox power limits commands. */
		if (xe_hwmon_pcode_read_power_limit(hwmon, PL1_HWMON_ATTR, CHANNEL_CARD,
						    &hwmon->pl1_on_boot[CHANNEL_CARD]) |
		    xe_hwmon_pcode_read_power_limit(hwmon, PL1_HWMON_ATTR, CHANNEL_PKG,
						    &hwmon->pl1_on_boot[CHANNEL_PKG]) |
		    xe_hwmon_pcode_read_power_limit(hwmon, PL2_HWMON_ATTR, CHANNEL_CARD,
						    &hwmon->pl2_on_boot[CHANNEL_CARD]) |
		    xe_hwmon_pcode_read_power_limit(hwmon, PL2_HWMON_ATTR, CHANNEL_PKG,
						    &hwmon->pl2_on_boot[CHANNEL_PKG])) {
			drm_warn(&hwmon->xe->drm,
				 "Failed to read power limits, check GPU firmware !\n");
		} else {
			drm_info(&hwmon->xe->drm, "Using mailbox commands for power limits\n");
			/* Write default limits to read from pcode from now on. */
			if (xe_hwmon_pcode_set_default_power_limits(hwmon))
				drm_warn(&hwmon->xe->drm, "Failed to set default power limits\n");
			hwmon->scl_shift_power = PWR_UNIT;
			hwmon->scl_shift_energy = ENERGY_UNIT;
			hwmon->scl_shift_time = TIME_UNIT;
		}
	} else {
		drm_info(&hwmon->xe->drm, "Using register for power limits\n");
		/*
		 * The contents of register PKG_POWER_SKU_UNIT do not change,
		 * so read it once and store the shift values.
		 */
		pkg_power_sku_unit = xe_hwmon_get_reg(hwmon, REG_PKG_POWER_SKU_UNIT, 0);
		if (xe_reg_is_valid(pkg_power_sku_unit)) {
			val_sku_unit = xe_mmio_read32(mmio, pkg_power_sku_unit);
			hwmon->scl_shift_power = REG_FIELD_GET(PKG_PWR_UNIT, val_sku_unit);
			hwmon->scl_shift_energy = REG_FIELD_GET(PKG_ENERGY_UNIT, val_sku_unit);
			hwmon->scl_shift_time = REG_FIELD_GET(PKG_TIME_UNIT, val_sku_unit);
		}
	}
	/*
	 * Initialize 'struct xe_hwmon_energy_info', i.e. set fields to the
	 * first value of the energy register read
	 */
	for (channel = 0; channel < CHANNEL_MAX; channel++)
		if (xe_hwmon_is_visible(hwmon, hwmon_energy, hwmon_energy_input, channel))
			xe_hwmon_energy_get(hwmon, channel, &energy);

	if (hwmon->xe->info.has_fan_control) {
		xe_hwmon_get_fan_enable_mask(hwmon);

		/* Initialize 'struct xe_hwmon_fan_info' with initial fan register reading. */
		for (channel = 0; channel < FAN_MAX; channel++)
			if (IS_FAN_ENABLED(hwmon, channel))
				xe_hwmon_fan_input_read(hwmon, channel, &fan_speed);

		/* Fan control tables initialization */
		if (xe_hwmon_read_fan_control_info(hwmon, hwmon->fan_enable_mask))
			xe_warn(hwmon->xe, "Fan control tables are not available\n");
	}
	return xe_hwmon_init_temp_info(hwmon);
}

int xe_hwmon_register(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;
	struct xe_hwmon *hwmon;
	int ret;

	/* hwmon is available only for dGfx */
	if (!IS_DGFX(xe))
		return 0;

	/* hwmon is not available on VFs */
	if (IS_SRIOV_VF(xe))
		return 0;

	hwmon = devm_kzalloc_obj(dev, *hwmon);
	if (!hwmon)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &hwmon->hwmon_lock);
	if (ret)
		return ret;

	/* There's only one instance of hwmon per device */
	hwmon->xe = xe;
	xe->hwmon = hwmon;

	ret = xe_hwmon_get_preregistration_info(hwmon);
	if (ret)
		goto err_null_hwmon;

	drm_dbg(&xe->drm, "Register xe hwmon interface\n");

	hwmon->groups[0] = &hwmon_attrgroup;
	if (xe->info.has_fan_control) {
		xe_hwmon_init_curve_attrs(hwmon);
		hwmon->groups[1] = &hwmon->curve_group;
	}

	/*  hwmon_dev points to device hwmon<i> */
	hwmon->hwmon_dev = hwmon_device_register_with_info(dev, "xe", hwmon,
							   &hwmon_chip_info,
							   hwmon->groups);
	if (IS_ERR(hwmon->hwmon_dev)) {
		ret = PTR_ERR(hwmon->hwmon_dev);
		xe_log_err(xe, HWMON, ret, "Failed to register xe hwmon\n");
		goto err_null_hwmon;
	}

	return 0;

err_null_hwmon:
	xe->hwmon = NULL;
	return ret;
}

/**
 * xe_hwmon_unregister - Remove the hwmon interface
 * @xe: xe device
 *
 * Called before the device is unplugged, so that no hwmon attribute can
 * be accessed afterwards.
 */
void xe_hwmon_unregister(struct xe_device *xe)
{
	if (!xe->hwmon)
		return;

	hwmon_device_unregister(xe->hwmon->hwmon_dev);
	xe->hwmon = NULL;
}

/**
 * xe_hwmon_suspend - Save hwmon state before suspend
 * @xe: xe device instance
 * @runtime: true for runtime suspend, false for system suspend or FLR prepare
 *
 * Save any hwmon runtime state that must survive PM suspend transitions.
 */
void xe_hwmon_suspend(struct xe_device *xe, bool runtime)
{
	struct xe_hwmon *hwmon = xe->hwmon;
	struct xe_tile *root_tile;
	int fan;

	if (!hwmon || !xe->info.has_fan_control)
		return;

	/* Runtime suspend keeps the fan tables unless the device enters d3cold. */
	if (runtime && !xe->d3cold.allowed) {
		xe_dbg(xe, "d3cold not allowed, skipping fan user-table snapshot\n");
		return;
	}

	root_tile = xe_device_get_root_tile(hwmon->xe);

	mutex_lock(&hwmon->hwmon_lock);

	for (fan = 0; fan < FAN_MAX; fan++) {
		struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
		struct fan_control_point points[MAX_FAN_CONTROL_POINTS];
		u32 point_count = 0;
		u32 point;
		int ret;

		if (!IS_FAN_ENABLED(hwmon, fan))
			continue;

		fi->restore_user_table = false;
		if (fi->pwm_enable_mode == XE_FAN_PWM_AUTO_STOCK_TABLE)
			continue;

		ret = xe_hwmon_get_fan_point_count(hwmon, fan, &point_count, USER_FAN_TABLE);
		if (ret || point_count < MIN_FAN_CONTROL_POINTS) {
			xe_warn(hwmon->xe,
				"fan %d user table point count before suspend invalid, ret=%d, count=%u\n",
				fan, ret, point_count);
			continue;
		}

		for (point = 0; point < point_count; point++) {
			u32 fcp = FCP_INDEX(USER_FAN_TABLE, point);

			ret = xe_pcode_read(root_tile, PCODE_MBOX(FAN_SPEED_CONTROL,
								  FSC_READ_FAN_TABLE, fan),
					    &fcp, NULL);
			if (ret) {
				xe_warn(hwmon->xe,
					"failed to read fan %d user table point %u before suspend, ret=%d\n",
					fan, point, ret);
				break;
			}

			points[point].temp =
				REG_FIELD_GET(FAN_CONTROL_POINT_TEMP_MASK, fcp);
			points[point].speed =
				REG_FIELD_GET(FAN_CONTROL_POINT_SPEED_MASK, fcp);
		}
		if (ret)
			continue;

		memcpy(fi->fan_table[USER_FAN_TABLE].fcp, points,
		       point_count * sizeof(points[0]));
		fi->fan_table[USER_FAN_TABLE].point_count = point_count;
		fi->restore_user_table = true;
		xe_dbg(hwmon->xe,
		       "fan %d user table snapshot taken before suspend, point_count=%u\n", fan,
		       point_count);
	}

	mutex_unlock(&hwmon->hwmon_lock);
}

/**
 * xe_hwmon_resume - Restore hwmon state after resume
 * @xe: xe device instance
 * @runtime: true for runtime resume, false for system resume or FLR done
 *
 * Restore any hwmon runtime state that was saved across suspend.
 */
void xe_hwmon_resume(struct xe_device *xe, bool runtime)
{
	struct xe_hwmon *hwmon = xe->hwmon;
	int fan;

	if (!hwmon || !xe->info.has_fan_control)
		return;

	if (runtime && !xe->d3cold.allowed) {
		xe_dbg(xe, "d3cold not allowed, skipping fan user-table restore\n");
		return;
	}

	mutex_lock(&hwmon->hwmon_lock);

	for (fan = 0; fan < FAN_MAX; fan++) {
		struct xe_hwmon_fan_info *fi = &hwmon->fi[fan];
		int ret;

		if (!IS_FAN_ENABLED(hwmon, fan))
			continue;

		if (!fi->restore_user_table) {
			xe_dbg(xe, "fan %d user table restore not required\n", fan);
			continue;
		}

		/* Re-program cached user-table settings for fans active before suspend. */
		ret = xe_hwmon_activate_user_fan_table(hwmon, fan,
						       fi->is_full_speed,
						       USER_FAN_TABLE);
		if (ret) {
			xe_warn(xe, "failed to restore fan %d user table after resume, ret=%d\n",
				fan, ret);
			continue;
		}

		xe_dbg(xe, "fan %d user table restored\n", fan);
		fi->restore_user_table = false;
	}

	mutex_unlock(&hwmon->hwmon_lock);
}

/**
 * xe_hwmon_fan_update_post_lb - Update fan info after late binding
 * @xe: xe device instance
 *
 * This function reads the number of fans and their stock table after late binding.
 */
void xe_hwmon_fan_update_post_lb(struct xe_device *xe)
{
	struct xe_hwmon *hwmon = xe->hwmon;
	u32 old_mask;

	if (!hwmon || !xe->info.has_fan_control)
		return;

	mutex_lock(&hwmon->hwmon_lock);

	old_mask = hwmon->fan_enable_mask;
	if (xe_hwmon_get_fan_enable_mask(hwmon)) {
		xe_warn(hwmon->xe, "keeping fan enable mask %#x after late binding\n", old_mask);
		hwmon->fan_enable_mask = old_mask;
		goto unlock;
	}

	if (xe_hwmon_read_fan_control_info(hwmon, hwmon->fan_enable_mask & ~old_mask))
		xe_warn(hwmon->xe, "failed to read stock fan tables after late binding\n");

unlock:
	mutex_unlock(&hwmon->hwmon_lock);
}

MODULE_IMPORT_NS("INTEL_PMT_TELEMETRY");
