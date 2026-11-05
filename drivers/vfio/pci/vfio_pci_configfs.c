// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI configuration through configfs
 *
 * The host administrator can hide BAR ranges of a device, in addition to the
 * ones hidden by its driver, through /sys/kernel/config/vfio-pci/<device>/.
 */

#include <linux/configfs.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "vfio_pci_priv.h"

/*
 * Parse a "<bar>:<offset>:<size>" range, where the BAR index is decimal and
 * the offset and the size are hexadecimal, with an optional 0x prefix.
 */
static int vfio_pci_parse_bar_range(const char *str,
				    struct vfio_pci_bar_range *range)
{
	char *buf __free(kfree) = kstrdup(str, GFP_KERNEL);
	char *cur, *bar, *offset;

	if (!buf)
		return -ENOMEM;

	cur = strim(buf);
	bar = strsep(&cur, ":");
	offset = strsep(&cur, ":");

	if (!cur || kstrtouint(bar, 10, &range->bar) ||
	    kstrtou64(offset, 16, &range->offset) ||
	    kstrtou64(cur, 16, &range->size))
		return -EINVAL;

	return 0;
}

/*
 * Ranges hidden when a device is registered, configured in a configfs
 * directory named after the PCI device, which can be created before the
 * device exists, e.g. for a VF before SR-IOV is enabled. The configuration
 * can't change while the device is bound, so that it always matches the
 * ranges hidden on a bound device.
 */
struct vfio_pci_configfs_device {
	struct config_group group;
	/* protects the ranges and bound */
	struct mutex lock;
	struct vfio_pci_bar_range hidden_ranges[VFIO_PCI_MAX_HIDDEN_BAR_RANGES];
	unsigned int num_hidden_ranges;
	/* the device is bound and pins the directory */
	bool bound;
};

/* the devices bound to a VFIO driver, protected by the subsystem su_mutex */
static LIST_HEAD(vfio_pci_configfs_bound);

static struct vfio_pci_configfs_device *
to_vfio_pci_configfs_device(struct config_item *item)
{
	return container_of(to_config_group(item),
			    struct vfio_pci_configfs_device, group);
}

static ssize_t vfio_pci_configfs_hidden_bar_ranges_show(struct config_item *item,
							char *page)
{
	struct vfio_pci_configfs_device *cfg = to_vfio_pci_configfs_device(item);

	guard(mutex)(&cfg->lock);

	return vfio_pci_show_bar_ranges(cfg->hidden_ranges,
					cfg->num_hidden_ranges, page);
}

static ssize_t vfio_pci_configfs_store_bar_range(struct config_item *item,
						 const char *page, size_t len,
						 bool hide)
{
	struct vfio_pci_configfs_device *cfg = to_vfio_pci_configfs_device(item);
	struct vfio_pci_bar_range range;
	int ret;

	ret = vfio_pci_parse_bar_range(page, &range);
	if (ret)
		return ret;

	guard(mutex)(&cfg->lock);

	if (cfg->bound)
		return -EBUSY;

	if (hide)
		ret = vfio_pci_add_bar_range(cfg->hidden_ranges,
					     &cfg->num_hidden_ranges, &range);
	else
		ret = vfio_pci_del_bar_range(cfg->hidden_ranges,
					     &cfg->num_hidden_ranges, &range);

	return ret ?: len;
}

static ssize_t vfio_pci_configfs_hide_bar_range_store(struct config_item *item,
						      const char *page, size_t len)
{
	return vfio_pci_configfs_store_bar_range(item, page, len, true);
}

static ssize_t vfio_pci_configfs_unhide_bar_range_store(struct config_item *item,
							const char *page, size_t len)
{
	return vfio_pci_configfs_store_bar_range(item, page, len, false);
}

CONFIGFS_ATTR_RO(vfio_pci_configfs_, hidden_bar_ranges);
CONFIGFS_ATTR_WO(vfio_pci_configfs_, hide_bar_range);
CONFIGFS_ATTR_WO(vfio_pci_configfs_, unhide_bar_range);

static struct configfs_attribute *vfio_pci_configfs_device_attrs[] = {
	&vfio_pci_configfs_attr_hidden_bar_ranges,
	&vfio_pci_configfs_attr_hide_bar_range,
	&vfio_pci_configfs_attr_unhide_bar_range,
	NULL
};

static void vfio_pci_configfs_device_release(struct config_item *item)
{
	struct vfio_pci_configfs_device *cfg = to_vfio_pci_configfs_device(item);

	mutex_destroy(&cfg->lock);
	kfree(cfg);
}

static struct configfs_item_operations vfio_pci_configfs_device_item_ops = {
	.release = vfio_pci_configfs_device_release,
};

static const struct config_item_type vfio_pci_configfs_device_type = {
	.ct_item_ops = &vfio_pci_configfs_device_item_ops,
	.ct_attrs = vfio_pci_configfs_device_attrs,
	.ct_owner = THIS_MODULE,
};

/* only the canonical name, as returned by pci_name(), is looked up */
static int vfio_pci_configfs_check_name(const char *name)
{
	unsigned int domain, bus, slot, function;
	char canonical[sizeof("ffffffff:ff:1f.7")];

	if (sscanf(name, "%x:%x:%x.%x", &domain, &bus, &slot, &function) != 4 ||
	    bus > 0xff || slot > 0x1f || function > 7)
		return -EINVAL;

	scnprintf(canonical, sizeof(canonical), "%04x:%02x:%02x.%u",
		  domain, bus, slot, function);

	return strcmp(name, canonical) ? -EINVAL : 0;
}

static struct config_group *vfio_pci_configfs_make_group(struct config_group *group,
							 const char *name)
{
	struct vfio_pci_configfs_device *cfg;
	struct vfio_pci_core_device *vdev;
	int ret;

	ret = vfio_pci_configfs_check_name(name);
	if (ret)
		return ERR_PTR(ret);

	list_for_each_entry(vdev, &vfio_pci_configfs_bound, configfs_node) {
		if (!strcmp(pci_name(vdev->pdev), name))
			return ERR_PTR(-EBUSY);
	}

	cfg = kzalloc_obj(*cfg);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	mutex_init(&cfg->lock);
	config_group_init_type_name(&cfg->group, name,
				    &vfio_pci_configfs_device_type);

	return &cfg->group;
}

static struct configfs_group_operations vfio_pci_configfs_group_ops = {
	.make_group = vfio_pci_configfs_make_group,
};

static const struct config_item_type vfio_pci_configfs_type = {
	.ct_group_ops = &vfio_pci_configfs_group_ops,
	.ct_owner = THIS_MODULE,
};

static struct configfs_subsystem vfio_pci_configfs = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "vfio-pci",
			.ci_type = &vfio_pci_configfs_type,
		},
	},
};

/*
 * Hide the ranges configured for the device being bound and keep the
 * configuration from changing, including the creation and removal of the
 * directory, until it is unbound.
 */
int vfio_pci_configfs_bind(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_configfs_device *cfg;
	struct config_item *item;
	int ret;

	scoped_guard(mutex, &vfio_pci_configfs.su_mutex) {
		item = config_group_find_item(&vfio_pci_configfs.su_group,
					      pci_name(vdev->pdev));
		list_add(&vdev->configfs_node, &vfio_pci_configfs_bound);
	}
	if (!item)
		return 0;

	cfg = to_vfio_pci_configfs_device(item);

	ret = configfs_depend_item(&vfio_pci_configfs, item);
	if (ret) {
		pci_err(vdev->pdev, "configfs directory being created or removed (%pe)\n",
			ERR_PTR(ret));
		ret = -EBUSY;
		goto out_unbind;
	}

	scoped_guard(mutex, &cfg->lock) {
		if (cfg->num_hidden_ranges && !vdev->hidden_ranges_supported) {
			pci_err(vdev->pdev, "%s doesn't support hiding the BAR ranges configured through configfs\n",
				vdev->vdev.ops->name);
			ret = -EOPNOTSUPP;
		} else {
			ret = __vfio_pci_hide_bar_ranges(vdev, cfg->hidden_ranges,
							 cfg->num_hidden_ranges,
							 "configfs");
		}
		cfg->bound = !ret;
	}
	if (ret) {
		configfs_undepend_item(item);
		goto out_unbind;
	}

	config_item_put(item);

	return 0;

out_unbind:
	config_item_put(item);
	scoped_guard(mutex, &vfio_pci_configfs.su_mutex)
		list_del(&vdev->configfs_node);

	return ret;
}

void vfio_pci_configfs_unbind(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_configfs_device *cfg;
	struct config_item *item;
	bool pinned;

	scoped_guard(mutex, &vfio_pci_configfs.su_mutex) {
		list_del(&vdev->configfs_node);
		item = config_group_find_item(&vfio_pci_configfs.su_group,
					      pci_name(vdev->pdev));
	}
	if (!item)
		return;

	cfg = to_vfio_pci_configfs_device(item);

	scoped_guard(mutex, &cfg->lock) {
		pinned = cfg->bound;
		cfg->bound = false;
	}
	if (pinned)
		configfs_undepend_item(item);

	config_item_put(item);
}

int vfio_pci_configfs_init(void)
{
	int ret;

	config_group_init(&vfio_pci_configfs.su_group);
	mutex_init(&vfio_pci_configfs.su_mutex);

	ret = configfs_register_subsystem(&vfio_pci_configfs);
	if (ret)
		mutex_destroy(&vfio_pci_configfs.su_mutex);

	return ret;
}

void vfio_pci_configfs_exit(void)
{
	configfs_unregister_subsystem(&vfio_pci_configfs);
	mutex_destroy(&vfio_pci_configfs.su_mutex);
}
