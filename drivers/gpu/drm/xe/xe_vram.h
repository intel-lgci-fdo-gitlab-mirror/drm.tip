/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_VRAM_H_
#define _XE_VRAM_H_

#include <linux/types.h>

#include "xe_vram_types.h"

struct xe_device;
struct xe_tile;
struct ttm_resource;
struct ttm_place;

struct xe_vram_region *xe_map_resource_to_region(struct ttm_resource *res);
int xe_vram_probe(struct xe_device *xe);

struct xe_vram_region *xe_vram_region_alloc_device(struct xe_device *xe);
struct xe_vram_region *xe_vram_region_alloc_tile(struct xe_device *xe, u8 id);
struct xe_vram_pool *xe_vram_region_default_pool(struct xe_vram_region *vram);
struct xe_vram_pool *xe_vram_pool_create(struct xe_vram_region *vram,
					 enum xe_vram_pool_purpose purpose, u64 size);
int xe_vram_tile_alloc_kernel(struct xe_tile *tile);

void xe_vram_pool_place(const struct xe_vram_pool *pool, struct ttm_place *place);
struct xe_vram_pool *xe_vram_region_addr_to_pool(struct xe_vram_region *vram, u64 addr);
resource_size_t xe_vram_pool_usable_size(const struct xe_vram_pool *pool);

resource_size_t xe_vram_region_io_start(const struct xe_vram_region *vram);
resource_size_t xe_vram_region_io_size(const struct xe_vram_region *vram);
resource_size_t xe_vram_region_dpa_base(const struct xe_vram_region *vram);
resource_size_t xe_vram_region_usable_size(const struct xe_vram_region *vram);
resource_size_t xe_vram_region_actual_physical_size(const struct xe_vram_region *vram);

#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_MEM)
int xe_vram_reserve_memtest_bo(struct xe_device *xe);
void xe_vram_free_memtest_bos(struct xe_device *xe);
int xe_vram_memtest(struct xe_device *xe);
#else
static inline int xe_vram_reserve_memtest_bo(struct xe_device *xe) { return 0; }
static inline void xe_vram_free_memtest_bos(struct xe_device *xe) {}
static inline int xe_vram_memtest(struct xe_device *xe) { return 0; }
#endif

#endif
