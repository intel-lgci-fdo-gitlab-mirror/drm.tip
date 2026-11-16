/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_H_
#define _XE_TTM_VRAM_MGR_H_

#include "xe_ttm_vram_mgr_types.h"
#include "xe_vram_types.h"

enum dma_data_direction;
struct xe_device;
struct xe_tile;
struct xe_vram_region;

int xe_ttm_vram_mgr_init(struct xe_device *xe, struct xe_vram_region *vram);
void xe_ttm_vram_debugfs_init(struct xe_device *xe, struct dentry *root);
int xe_ttm_vram_mgr_alloc_sgt(struct xe_device *xe,
			      struct ttm_resource *res,
			      u64 offset, u64 length,
			      struct device *dev,
			      enum dma_data_direction dir,
			      struct sg_table **sgt);
void xe_ttm_vram_mgr_free_sgt(struct device *dev, enum dma_data_direction dir,
			      struct sg_table *sgt);

u64 xe_ttm_vram_get_pool_avail(struct xe_vram_pool *pool);
void xe_ttm_vram_get_general_pool_sizes(struct ttm_resource_manager *man,
					u64 *size, u64 *cpu_visible_size);
void xe_ttm_vram_get_general_pool_used(struct ttm_resource_manager *man,
				       u64 *used, u64 *used_visible);
int xe_ttm_vram_handle_addr_fault(struct xe_device *xe, u64 addr);
int xe_ttm_vram_inject_fault(struct xe_device *xe);

static inline struct xe_ttm_vram_mgr_resource *
to_xe_ttm_vram_mgr_resource(struct ttm_resource *res)
{
	return container_of(res, struct xe_ttm_vram_mgr_resource, base);
}

static inline struct xe_ttm_vram_mgr *
to_xe_ttm_vram_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct xe_ttm_vram_mgr, manager);
}

/**
 * xe_ttm_vram_mgr_block_offset() - VRAM region relative offset of a block
 * @part: Partition @block was allocated from
 * @block: The buddy block
 *
 * A VRAM region may be split across several buddy allocators, and the offset
 * tracked by a partition's buddy allocator is relative to the start of that
 * partition. A raw &gpu_buddy_block is therefore not enough on its own to
 * locate the memory it describes, so the owning partition has to be supplied
 * by the caller. Every block reachable outside this layer hangs off a
 * &xe_ttm_vram_mgr_resource, so use its @part.
 *
 * Use this rather than gpu_buddy_block_offset() anywhere an offset within the
 * VRAM region (i.e. a DPA relative to the region base) is needed.
 *
 * Return: Offset of @block relative to the start of the VRAM region.
 */
static inline u64
xe_ttm_vram_mgr_block_offset(const struct xe_ttm_vram_mgr_part *part,
			     const struct gpu_buddy_block *block)
{
	return part->pool->offset + gpu_buddy_block_offset(block);
}

/**
 * xe_ttm_vram_mgr_block_size() - Size of a buddy block
 * @part: Partition @block was allocated from
 * @block: The buddy block
 *
 * Wrapper around gpu_buddy_block_size() taking the partition @block was
 * allocated from, see xe_ttm_vram_mgr_block_offset().
 *
 * Return: Size of @block in bytes.
 */
static inline u64
xe_ttm_vram_mgr_block_size(struct xe_ttm_vram_mgr_part *part,
			   struct gpu_buddy_block *block)
{
	return gpu_buddy_block_size(&part->mm, block);
}

#endif
