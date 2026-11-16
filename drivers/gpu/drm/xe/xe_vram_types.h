/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_VRAM_TYPES_H_
#define _XE_VRAM_TYPES_H_

#if IS_ENABLED(CONFIG_DRM_XE_PAGEMAP)
#include <drm/drm_pagemap.h>
#endif

#include "xe_ttm_vram_mgr_types.h"

struct xe_device;
struct xe_migrate;

/**
 * enum xe_vram_binding - VRAM region binding types
 * @XE_VRAM_BINDING_NONE: no binding assigned, default value of a zeroed region
 * @XE_VRAM_BINDING_DEVICE: device-level region aggregating all tile VRAM regions
 * @XE_VRAM_BINDING_TILE: VRAM region bound to a tile
 */
enum xe_vram_binding {
	XE_VRAM_BINDING_NONE = 0,
	XE_VRAM_BINDING_DEVICE,
	XE_VRAM_BINDING_TILE,
};

/**
 * enum xe_vram_pool_purpose - What a VRAM pool serves
 * @XE_VRAM_POOL_GENERAL: general-purpose memory, available to userspace
 * @XE_VRAM_POOL_KERNEL: kernel-only memory, never exposed to userspace
 */
enum xe_vram_pool_purpose {
	XE_VRAM_POOL_GENERAL = 0,
	XE_VRAM_POOL_KERNEL,
};

/**
 * XE_VRAM_MAX_PARTITIONS - Max partitions a single VRAM region can be split into
 *
 * A region currently holds its default pool and at most one pool dedicated
 * to another purpose. The TTM manager mirrors the partitions of a region one
 * by one, so a region cannot hold more of them than the manager can back.
 */
#define XE_VRAM_MAX_PARTITIONS	XE_TTM_VRAM_MGR_MAX_PARTS

/**
 * DOC: VRAM pools, regions and partitions
 *
 * Allocation paths place memory through a struct xe_vram_pool. A pool names
 * a purpose (general-purpose, kernel-only ...) and resolves to a TTM
 * placement plus the address range serving it, see xe_vram_pool_place().
 * Whether that range is the whole region, or one of several partitions
 * sharing a region, is a backend detail pool consumers do not need to know.
 *
 * A struct xe_vram_region is the physical carrier: it owns the BAR mapping,
 * the DPA range and the TTM resource manager. A non-aggregate region
 * initially contains one partition spanning its whole usable range. That
 * partition is the region's default, general-purpose pool. It always
 * occupies partitions[0].
 *
 * Each partition is described by a struct xe_vram_pool embedded in the
 * region, so a pointer published during initialization remains valid if a
 * later split changes the pool's offset and size. A split never changes a
 * pool's purpose and may only happen before the region's TTM manager is
 * initialized.
 *
 * The device aggregate described below is the only region type that has no
 * manager and no partitions.
 *
 * The id shown below is the VRAM region instance id. For a tile region it
 * is the id of the tile, which the uAPI relies on. The pair (binding, id)
 * must uniquely identify a region.
 *
 * Partitioned region:
 * A tile's VRAM region can be split so that a pool dedicated to some purpose,
 * e.g. kernel-only, gets a range of its own and the default pool keeps the
 * rest. The dedicated partition is carved out at the start of the region, at
 * the lowest addresses, and takes the next free index. The default partition
 * keeps index 0 and is moved above the dedicated range. A tile without such
 * a reservation keeps one partition spanning the whole usable range::
 *
 *         xe_vram_region "tile0" (VRAM region id 0, binding TILE)
 *         ,-------------------------------------------------------------.
 *         | ttm: struct xe_ttm_vram_mgr (one manager, shared)           |
 *         | .---------------------------------------------------------. |
 *         | |  partitions[1]            |  partitions[0]              | |
 *         | |  offset 0                 |  offset pool_size           | |
 *         | |  size   pool_size         |  size   usable - pool_size  | |
 *         | `---------------------------------------------------------' |
 *         `-------------------------------------------------------------'
 *                  ^                               ^
 *                  |                               |
 *             dedicated pool                  general-purpose pool
 *
 *         xe_vram_region "tile1" (VRAM region id 1, binding TILE)
 *         ,-------------------------------------------------------------.
 *         | ttm: struct xe_ttm_vram_mgr (one manager)                   |
 *         | .---------------------------------------------------------. |
 *         | |  partitions[0]                                          | |
 *         | |  offset 0                                               | |
 *         | |  size   whole usable range                              | |
 *         | `---------------------------------------------------------' |
 *         `-------------------------------------------------------------'
 *                                     ^
 *                                     |
 *                            general-purpose pool
 *
 * Device aggregate:
 * A device-level aggregate region is unrelated to the tile regions. It is
 * never backed by a resource manager and has no partitions. It only
 * aggregates the sizes and DPA ranges of every tile for reporting purposes.
 * Nothing is ever allocated from it::
 *
 *         xe_vram_region "device" (VRAM region id 0, binding DEVICE)
 *         ,-------------------------------------------------------------.
 *         | ttm:        unused, not backed by any resource manager      |
 *         | partitions: none, this region is never allocated from       |
 *         | size:       total VRAM summed across all tiles              |
 *         `-------------------------------------------------------------'
 */

/**
 * struct xe_vram_pool - VRAM range used for a given allocation purpose
 *
 * VRAM allocations are placed through this frontend. It identifies an
 * allocation purpose and resolves it to a TTM placement and the address
 * range serving it. Whether that range is a whole region of its own - and
 * thus has its own placement, mapping and address range - or one of several
 * partitions of a region is a backend detail the allocation paths do not
 * need to know, see xe_vram_pool_place().
 *
 * A pool stays valid for the lifetime of its region and its purpose is fixed
 * once published.
 */
struct xe_vram_pool {
	/** @region: region this pool belongs to */
	struct xe_vram_region *region;
	/**
	 * @part: TTM partition backing this pool, or NULL until the manager
	 * is initialized
	 */
	struct xe_ttm_vram_mgr_part *part;
	/** @offset: byte offset of the pool from the start of @region */
	u64 offset;
	/** @size: size of the pool in bytes */
	u64 size;
	/** @purpose: allocation purpose served by this pool */
	enum xe_vram_pool_purpose purpose;
};

/**
 * struct xe_vram_region - memory region structure
 * This is used to describe a memory region in xe
 * device, such as HBM memory or CXL extension memory.
 */
struct xe_vram_region {
	/** @xe: Back pointer to xe device */
	struct xe_device *xe;
	/**
	 * @id: VRAM region instance id
	 *
	 * The pair (@binding, @id) must uniquely identify a VRAM region.
	 */
	u8 id;
	/** @binding: VRAM region instance binding */
	enum xe_vram_binding binding;
	/** @io_start: IO start address of this VRAM instance */
	resource_size_t io_start;
	/**
	 * @io_size: IO size of this VRAM instance
	 *
	 * This represents how much of this VRAM we can access
	 * via the CPU through the VRAM BAR. This can be smaller
	 * than @usable_size, in which case only part of VRAM is CPU
	 * accessible (typically the first 256M). This
	 * configuration is known as small-bar.
	 */
	resource_size_t io_size;
	/** @dpa_base: This memory regions's DPA (device physical address) base */
	resource_size_t dpa_base;
	/**
	 * @usable_size: usable size of VRAM
	 *
	 * Usable size of VRAM excluding reserved portions
	 * (e.g stolen mem)
	 */
	resource_size_t usable_size;
	/**
	 * @actual_physical_size: Actual VRAM size
	 *
	 * Actual VRAM size including reserved portions
	 * (e.g stolen mem)
	 */
	resource_size_t actual_physical_size;
	/**
	 * @partitions: partitions this region is split into
	 *
	 * A non-aggregate region initially has @partitions[0] spanning its
	 * whole usable range. It is the region's default, general-purpose
	 * pool. An optional split may adjust its range and create another
	 * partition before the TTM resource manager is initialized.
	 *
	 * A device aggregate region has no partitions. Allocation paths do
	 * not take a region pointer, they take a &xe_vram_pool pointing into
	 * this array, see the DOC above.
	 */
	struct xe_vram_pool partitions[XE_VRAM_MAX_PARTITIONS];
	/**
	 * @num_partitions: number of valid entries in @partitions
	 *
	 * Zero for a device aggregate region.
	 */
	unsigned int num_partitions;
	/** @mapping: pointer to VRAM mappable space */
	void __iomem *mapping;
	/** @ttm: VRAM TTM manager */
	struct xe_ttm_vram_mgr ttm;
	/** @placement: TTM placement dedicated for this region */
	u32 placement;
#if IS_ENABLED(CONFIG_DRM_XE_PAGEMAP)
	/** @migrate: Back pointer to migrate */
	struct xe_migrate *migrate;
	/** @dpagemap_cache: drm_pagemap cache. */
	struct drm_pagemap_cache *dpagemap_cache;
#endif
};

#endif
