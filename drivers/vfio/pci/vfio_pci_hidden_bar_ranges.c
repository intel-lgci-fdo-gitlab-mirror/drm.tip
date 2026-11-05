// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI BAR ranges hidden from the VFIO user
 *
 * Some devices expose registers that must stay under control of the host,
 * e.g. device management registers, in the same BAR as the functional ones.
 * The ranges of such registers can be hidden from the VFIO user: region
 * reads of them are filled with ones and writes to them are dropped, the
 * pages containing them are never mapped, and neither ioeventfds nor
 * dma-bufs may cover them.
 *
 * The ranges are hidden by the driver of the device and the host
 * administrator can hide more of them through configfs, see
 * vfio_pci_configfs.c.
 */

#include <linux/device.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/vfio.h>

#include "vfio_pci_priv.h"

static bool vfio_pci_bar_ranges_overlap(const struct vfio_pci_bar_range *range,
					unsigned int bar, u64 offset, u64 size)
{
	return range->bar == bar && offset < range->offset + range->size &&
	       range->offset < offset + size;
}

bool __vfio_pci_bar_range_hidden(struct vfio_pci_core_device *vdev,
				 unsigned int bar, u64 offset, u64 size)
{
	unsigned int i;

	/* treat an overflowing range as hidden, the callers clamp it if needed */
	if (size > U64_MAX - offset)
		return true;

	for (i = 0; i < vdev->num_hidden_ranges; i++) {
		if (vfio_pci_bar_ranges_overlap(&vdev->hidden_ranges[i], bar,
						offset, size))
			return true;
	}

	return false;
}

static int vfio_pci_check_bar_range(const struct vfio_pci_bar_range *range)
{
	if (range->bar >= PCI_STD_NUM_BARS || !range->size ||
	    range->size > U64_MAX - range->offset)
		return -EINVAL;

	return 0;
}

int vfio_pci_add_bar_range(struct vfio_pci_bar_range *ranges,
			   unsigned int *num,
			   const struct vfio_pci_bar_range *range)
{
	unsigned int i;
	int ret;

	ret = vfio_pci_check_bar_range(range);
	if (ret)
		return ret;

	for (i = 0; i < *num; i++) {
		if (vfio_pci_bar_ranges_overlap(&ranges[i], range->bar,
						range->offset, range->size))
			return -EEXIST;
	}

	if (*num == VFIO_PCI_MAX_HIDDEN_BAR_RANGES)
		return -ENOSPC;

	for (i = *num; i > 0; i--) {
		if (ranges[i - 1].bar < range->bar ||
		    (ranges[i - 1].bar == range->bar &&
		     ranges[i - 1].offset < range->offset))
			break;

		ranges[i] = ranges[i - 1];
	}

	ranges[i] = *range;
	(*num)++;

	return 0;
}

static void vfio_pci_remove_bar_range(struct vfio_pci_bar_range *ranges,
				      unsigned int *num, unsigned int i)
{
	memmove(&ranges[i], &ranges[i + 1], (*num - i - 1) * sizeof(*ranges));
	(*num)--;
}

ssize_t vfio_pci_show_bar_ranges(const struct vfio_pci_bar_range *ranges,
				 unsigned int num, char *buf)
{
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < num; i++)
		len += sysfs_emit_at(buf, len, "%u:%#llx:%#llx\n", ranges[i].bar,
				     ranges[i].offset, ranges[i].size);

	return len;
}

static int vfio_pci_hide_bar_range(struct vfio_pci_core_device *vdev,
				   const struct vfio_pci_bar_range *range)
{
	struct vfio_pci_bar_range merged = *range;
	struct pci_dev *pdev = vdev->pdev;
	unsigned int i = 0;
	int ret;

	ret = vfio_pci_check_bar_range(range);
	if (ret)
		return ret;

	if (!(pci_resource_flags(pdev, range->bar) & IORESOURCE_MEM) ||
	    !pci_resource_start(pdev, range->bar) ||
	    range->size > pci_resource_len(pdev, range->bar) ||
	    range->offset > pci_resource_len(pdev, range->bar) - range->size)
		return -EINVAL;

	while (i < vdev->num_hidden_ranges) {
		const struct vfio_pci_bar_range *hidden = &vdev->hidden_ranges[i];
		u64 end;

		if (!vfio_pci_bar_ranges_overlap(hidden, merged.bar,
						 merged.offset, merged.size)) {
			i++;
			continue;
		}

		end = max(hidden->offset + hidden->size,
			  merged.offset + merged.size);
		merged.offset = min(hidden->offset, merged.offset);
		merged.size = end - merged.offset;
		vfio_pci_remove_bar_range(vdev->hidden_ranges,
					  &vdev->num_hidden_ranges, i);
	}

	return vfio_pci_add_bar_range(vdev->hidden_ranges,
				      &vdev->num_hidden_ranges, &merged);
}

int __vfio_pci_hide_bar_ranges(struct vfio_pci_core_device *vdev,
			       const struct vfio_pci_bar_range *ranges,
			       unsigned int num, const char *source)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num; i++) {
		ret = vfio_pci_hide_bar_range(vdev, &ranges[i]);
		if (ret) {
			pci_err(vdev->pdev, "Failed to hide BAR range %u:%#llx:%#llx from %s (%pe)\n",
				ranges[i].bar, ranges[i].offset, ranges[i].size,
				source, ERR_PTR(ret));
			return ret;
		}
	}

	return 0;
}

/**
 * vfio_pci_core_hide_bar_ranges - hide BAR ranges from the VFIO user
 * @vdev: device the ranges belong to
 * @ranges: ranges to hide, of the memory BARs of the device
 * @num: number of entries in @ranges
 *
 * Must be called before vfio_pci_core_register_device(), so that the ranges
 * are hidden before the device can be opened. The ranges configured by the
 * host administrator through configfs, if CONFIG_VFIO_PCI_CONFIGFS is enabled,
 * are hidden by vfio_pci_core_register_device() after the ones of the driver.
 * The ranges are hidden by the accesses done through vfio-pci-core: region
 * reads and writes, region infos, mmaps, ioeventfds and dma-bufs. The driver
 * must declare that it doesn't access its memory BARs in any other way by
 * setting &vfio_pci_core_device.hidden_ranges_supported first.
 *
 * The accesses crossing the boundaries of a hidden range are split, so the
 * range should match the access granularity of the device, e.g. its register
 * width. A range overlapping already hidden ones is merged with them, at most
 * %VFIO_PCI_MAX_HIDDEN_BAR_RANGES ranges can be hidden on a device after
 * merging.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int vfio_pci_core_hide_bar_ranges(struct vfio_pci_core_device *vdev,
				  const struct vfio_pci_bar_range *ranges,
				  unsigned int num)
{
	if (WARN_ON(!vdev->hidden_ranges_supported))
		return -EINVAL;

	return __vfio_pci_hide_bar_ranges(vdev, ranges, num, vdev->vdev.ops->name);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_hide_bar_ranges);

/*
 * Advertise only the parts of the BAR outside of the pages containing the
 * hidden ranges as mmappable, so that the VFIO user traps accesses to those
 * pages instead of trying to mmap the whole BAR and getting SIGBUS.
 */
int vfio_pci_hidden_add_sparse_mmap(struct vfio_pci_core_device *vdev,
				    struct vfio_region_info *info,
				    struct vfio_info_cap *caps)
{
	struct vfio_region_info_cap_sparse_mmap *sparse;
	u64 start = 0, end = PAGE_ALIGN(info->size);
	u64 hidden_start, hidden_end;
	unsigned int i, nr_areas = 0;
	int ret = 0;

	if (!vfio_pci_bar_range_hidden(vdev, info->index, 0, info->size))
		return 0;

	sparse = kzalloc_flex(*sparse, areas, vdev->num_hidden_ranges + 1);
	if (!sparse)
		return -ENOMEM;

	for (i = 0; i < vdev->num_hidden_ranges; i++) {
		const struct vfio_pci_bar_range *range = &vdev->hidden_ranges[i];

		if (range->bar != info->index)
			continue;

		hidden_start = ALIGN_DOWN(range->offset, PAGE_SIZE);
		hidden_end = PAGE_ALIGN(range->offset + range->size);

		if (hidden_start > start) {
			sparse->areas[nr_areas].offset = start;
			sparse->areas[nr_areas].size = hidden_start - start;
			nr_areas++;
		}
		/* ranges may share a page with the previous one */
		start = max(start, hidden_end);
	}

	if (start < end) {
		sparse->areas[nr_areas].offset = start;
		sparse->areas[nr_areas].size = end - start;
		nr_areas++;
	}

	if (!nr_areas) {
		info->flags &= ~VFIO_REGION_INFO_FLAG_MMAP;
		goto out;
	}

	sparse->nr_areas = nr_areas;
	sparse->header.id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
	sparse->header.version = 1;

	ret = vfio_info_add_capability(caps, &sparse->header,
				       struct_size(sparse, areas, nr_areas));
out:
	kfree(sparse);
	return ret;
}

static ssize_t hidden_bar_ranges_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct vfio_pci_core_device *vdev =
		container_of(dev, struct vfio_pci_core_device, vdev.device);

	return vfio_pci_show_bar_ranges(vdev->hidden_ranges,
					vdev->num_hidden_ranges, buf);
}
static DEVICE_ATTR_RO(hidden_bar_ranges);

static struct attribute *vfio_pci_hidden_bar_ranges_attrs[] = {
	&dev_attr_hidden_bar_ranges.attr,
	NULL
};

static const struct attribute_group vfio_pci_hidden_bar_ranges_group = {
	.attrs = vfio_pci_hidden_bar_ranges_attrs,
};

const struct attribute_group *vfio_pci_hidden_bar_ranges_groups[] = {
	&vfio_pci_hidden_bar_ranges_group,
	NULL
};

int vfio_pci_del_bar_range(struct vfio_pci_bar_range *ranges,
			   unsigned int *num,
			   const struct vfio_pci_bar_range *range)
{
	unsigned int i;

	for (i = 0; i < *num; i++) {
		if (ranges[i].bar == range->bar &&
		    ranges[i].offset == range->offset &&
		    ranges[i].size == range->size) {
			vfio_pci_remove_bar_range(ranges, num, i);
			return 0;
		}
	}

	return -ENOENT;
}
