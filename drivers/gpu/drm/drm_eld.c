// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/export.h>
#include <linux/pci.h>
#include <linux/unaligned.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>

#include "drm_crtc_internal.h"

/**
 * drm_eld_sad_get - get SAD from ELD to struct cea_sad
 * @eld: ELD buffer
 * @sad_index: SAD index
 * @cta_sad: destination struct cea_sad
 *
 * @return: 0 on success, or negative on errors
 */
int drm_eld_sad_get(const u8 *eld, int sad_index, struct cea_sad *cta_sad)
{
	const u8 *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_set(cta_sad, sad);

	return 0;
}
EXPORT_SYMBOL(drm_eld_sad_get);

/**
 * drm_eld_sad_set - set SAD to ELD from struct cea_sad
 * @eld: ELD buffer
 * @sad_index: SAD index
 * @cta_sad: source struct cea_sad
 *
 * @return: 0 on success, or negative on errors
 */
int drm_eld_sad_set(u8 *eld, int sad_index, const struct cea_sad *cta_sad)
{
	u8 *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_get(cta_sad, sad);

	return 0;
}
EXPORT_SYMBOL(drm_eld_sad_set);

/**
 * drm_eld_set_port_id - set the ELD Port_ID for a connector
 * @eld: ELD buffer
 * @connector: connector the ELD belongs to
 *
 * drm_edid_to_eld() leaves the 64-bit Port_ID field of the ELD baseline
 * block zero. Drivers can use this helper to fill it in with a value
 * that identifies the audio endpoint, so that userspace can associate an
 * ELD, and the PCM device it belongs to, with a DRM connector, even when
 * identical sinks are connected to multiple outputs.
 *
 * The Port_ID is stored in little-endian byte order, with the layout:
 *
 * - bits 63:48 - PCI domain of the DRM device, truncated to 16 bits
 * - bits 47:32 - PCI bus/device/function of the DRM device
 * - bits 31:0  - DRM connector object ID
 *
 * For non-PCI devices bits 63:32 are zero.
 *
 * The connector object ID is only stable for the lifetime of the
 * connector, so userspace must look it up at runtime, and not store it.
 */
void drm_eld_set_port_id(u8 *eld, const struct drm_connector *connector)
{
	struct device *dev = connector->dev->dev;
	u64 port_id = connector->base.id;

	if (dev && dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);

		port_id |= (u64)(pci_domain_nr(pdev->bus) & 0xffff) << 48 |
			   (u64)pci_dev_id(pdev) << 32;
	}

	put_unaligned_le64(port_id, &eld[DRM_ELD_PORT_ID]);
}
EXPORT_SYMBOL(drm_eld_set_port_id);
