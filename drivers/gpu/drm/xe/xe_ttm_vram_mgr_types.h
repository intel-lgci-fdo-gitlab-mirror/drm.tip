/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_TYPES_H_
#define _XE_TTM_VRAM_MGR_TYPES_H_

#include <linux/gpu_buddy.h>
#include <linux/sizes.h>
#include <drm/ttm/ttm_device.h>

struct xe_vram_pool;

/**
 * struct xe_ttm_vram_mgr_part - Partition of a VRAM region
 *
 * A VRAM region may be split into disjoint partitions, each managed by its
 * own buddy allocator. An allocation belongs to exactly one partition and
 * cannot span partition boundaries. This lets the manager allocate directly
 * from the requested partition without resorting to
 * %GPU_BUDDY_RANGE_ALLOCATION, which constrains the buddy allocation
 * algorithm.
 */
struct xe_ttm_vram_mgr_part {
	/**
	 * @pool: VRAM pool backed by this partition
	 *
	 * Once the manager is initialized, pool->part points back here.
	 */
	struct xe_vram_pool *pool;
	/** @mm: DRM buddy allocator which manages this partition */
	struct gpu_buddy mm;
	/** @used_visible: CPU visible bytes allocated from this partition */
	u64 used_visible;
};

/** XE_TTM_VRAM_MGR_MAX_PARTS - Max partitions of a single VRAM region */
#define XE_TTM_VRAM_MGR_MAX_PARTS	2

/**
 * XE_TTM_VRAM_MGR_PART_ALIGN - Required alignment of a partition base
 *
 * Buddy offsets are partition relative, so a partition base has to be
 * aligned to the largest VRAM page size in regular use, otherwise an aligned
 * buddy allocation is not aligned in the region.
 */
#define XE_TTM_VRAM_MGR_PART_ALIGN	SZ_2M

/**
 * struct xe_ttm_vram_mgr - Xe TTM VRAM manager
 *
 * Manages placement of TTM resource in VRAM.
 */
struct xe_ttm_vram_mgr {
	/** @manager: Base TTM resource manager */
	struct ttm_resource_manager manager;
	/** @parts: Partitions this VRAM region is split into */
	struct xe_ttm_vram_mgr_part parts[XE_TTM_VRAM_MGR_MAX_PARTS];
	/** @num_parts: Number of initialized entries in @parts */
	unsigned int num_parts;
	/** @offlined_pages: List of offlined pages */
	struct list_head offlined_pages;
	/** @n_offlined_pages: Number of offlined pages */
	u16 n_offlined_pages;
	/** @queued_pages: List of queued pages */
	struct list_head queued_pages;
	/** @n_queued_pages: Number of queued pages */
	u16 n_queued_pages;
	/** @visible_size: Probed size of the CPU visible portion */
	u64 visible_size;
	/** @visible_avail: CPU visible portion still unallocated */
	u64 visible_avail;
	/** @default_page_size: default page size */
	u64 default_page_size;
	/** @lock: protects allocations of VRAM */
	struct mutex lock;
	/** @mem_type: The TTM memory type */
	u32 mem_type;
};

/**
 * struct xe_ttm_vram_mgr_resource - Xe TTM VRAM resource
 */
struct xe_ttm_vram_mgr_resource {
	/** @base: Base TTM resource */
	struct ttm_resource base;
	/** @blocks: list of DRM buddy blocks */
	struct list_head blocks;
	/** @part: Partition owning @blocks, set on successful allocation */
	struct xe_ttm_vram_mgr_part *part;
	/** @used_visible_size: How many CPU visible bytes this resource is using */
	u64 used_visible_size;
	/** @flags: flags associated with the resource */
	unsigned long flags;
};

/**
 * enum xe_page_reserve_status - Buddy reservation status
 * @XE_PAGE_RESERVE_PENDING: reservation in progress
 * @XE_PAGE_RESERVE_FAIL: reservation failed
 */
enum xe_page_reserve_status {
	XE_PAGE_RESERVE_PENDING = 0,
	XE_PAGE_RESERVE_FAIL,
};

/**
 * struct xe_ttm_vram_offline_resource - Tracks a single offlined VRAM page
 */
struct xe_ttm_vram_offline_resource {
	/** @offlined_link: Link into mgr->offlined_pages */
	struct list_head offlined_link;
	/** @queued_link: Link into mgr->queued_pages */
	struct list_head queued_link;
	/** @blocks: Buddy blocks reserved for this page */
	struct list_head blocks;
	/** @part: Partition owning @blocks, set on successful reservation */
	struct xe_ttm_vram_mgr_part *part;
	/** @used_visible_size: CPU-visible bytes consumed */
	u64 used_visible_size;
	/** @addr: Faulty DPA reported by HW */
	u64 addr;
	/** @status: buddy reservation status */
	enum xe_page_reserve_status status;
};

#endif
