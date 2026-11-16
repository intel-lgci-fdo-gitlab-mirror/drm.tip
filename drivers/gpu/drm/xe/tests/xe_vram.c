// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <kunit/test.h>

#include "xe_kunit_helpers.h"
#include "xe_pci_test.h"

static int xe_vram_test_init(struct kunit *test)
{
	struct xe_pci_fake_data fake = {
		.platform = XE_BATTLEMAGE,
		.graphics_verx100 = 2001,
	};

	test->priv = &fake;
	xe_kunit_helper_xe_device_test_init(test);

	return 0;
}

static struct xe_vram_pool *tile_pool(struct kunit *test, struct xe_device *xe)
{
	struct xe_vram_region *vram = xe_vram_region_alloc_tile(xe, 0);
	struct xe_vram_pool *pool;

	KUNIT_ASSERT_NOT_NULL(test, vram);
	vram->usable_size = SZ_64M;
	vram->io_size = SZ_64M;
	pool = xe_vram_region_default_pool(vram);
	pool->size = SZ_64M;
	pool->purpose = XE_VRAM_POOL_GENERAL;
	vram->num_partitions = 1;

	return pool;
}

static void kernel_pool_defaults_to_general(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	struct xe_vram_pool *general = tile_pool(test, xe);
	struct xe_tile *tile = &xe->tiles[0];

	tile->mem.vram = general;
	tile->mem.kernel_vram = NULL;

	KUNIT_ASSERT_EQ(test, xe_vram_tile_alloc_kernel(tile), 0);
	KUNIT_EXPECT_PTR_EQ(test, tile->mem.kernel_vram, tile->mem.vram);
}

static void partition_carve(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	struct xe_vram_pool *general = tile_pool(test, xe);
	struct xe_vram_region *vram = general->region;
	struct xe_vram_pool *kernel0 = &vram->partitions[1];

	KUNIT_EXPECT_PTR_EQ(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_1M), NULL);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_64M), NULL);
	KUNIT_EXPECT_EQ(test, vram->num_partitions, 1u);

	KUNIT_ASSERT_NOT_NULL(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_8M));
	KUNIT_ASSERT_EQ(test, vram->num_partitions, 2u);
	KUNIT_EXPECT_EQ(test, kernel0->purpose, XE_VRAM_POOL_KERNEL);
	KUNIT_EXPECT_EQ(test, kernel0->offset, 0ull);
	KUNIT_EXPECT_EQ(test, kernel0->size, (u64)SZ_8M);
	KUNIT_EXPECT_EQ(test, general->purpose, XE_VRAM_POOL_GENERAL);
	KUNIT_EXPECT_EQ(test, general->offset, (u64)SZ_8M);
	KUNIT_EXPECT_EQ(test, general->size, (u64)(SZ_64M - SZ_8M));

	/* Both partitions keep sharing the region, its manager and its placement */
	KUNIT_EXPECT_PTR_EQ(test, kernel0->region, vram);
	KUNIT_EXPECT_PTR_EQ(test, general->region, vram);
	KUNIT_EXPECT_EQ(test, vram->placement, (u32)XE_PL_VRAM0);

	KUNIT_EXPECT_EQ(test, xe_vram_pool_usable_size(kernel0), (resource_size_t)SZ_8M);
	KUNIT_EXPECT_EQ(test, xe_vram_pool_usable_size(general),
			(resource_size_t)(SZ_64M - SZ_8M));
}

static void partition_limit(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	struct xe_vram_pool *general = tile_pool(test, xe);
	struct xe_vram_region *vram = general->region;
	struct xe_vram_pool *pool;
	unsigned int i;

	for (i = 1; i < XE_VRAM_MAX_PARTITIONS; i++) {
		pool = xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_2M);
		KUNIT_ASSERT_NOT_NULL(test, pool);
	}

	KUNIT_EXPECT_EQ(test, vram->num_partitions, XE_VRAM_MAX_PARTITIONS);
	KUNIT_EXPECT_PTR_EQ(test,
			    xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_2M),
			    NULL);
}

static void pool_place(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	struct xe_vram_pool *general = tile_pool(test, xe);
	struct xe_vram_region *vram = general->region;
	struct xe_vram_pool *kernel = &vram->partitions[1];
	struct ttm_place place = {};

	/* A pool spanning the region only resolves to its placement */
	xe_vram_pool_place(general, &place);
	KUNIT_EXPECT_EQ(test, place.mem_type, (u32)XE_PL_VRAM0);
	KUNIT_EXPECT_EQ(test, place.fpfn, 0u);
	KUNIT_EXPECT_EQ(test, place.lpfn, 0u);

	KUNIT_ASSERT_NOT_NULL(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_8M));

	place = (struct ttm_place) {};
	xe_vram_pool_place(kernel, &place);
	KUNIT_EXPECT_EQ(test, place.fpfn, 0u);
	KUNIT_EXPECT_EQ(test, place.lpfn, SZ_8M >> PAGE_SHIFT);

	place = (struct ttm_place) {};
	xe_vram_pool_place(general, &place);
	KUNIT_EXPECT_EQ(test, place.fpfn, SZ_8M >> PAGE_SHIFT);
	KUNIT_EXPECT_EQ(test, place.lpfn, SZ_64M >> PAGE_SHIFT);

	/* A caller supplied range, e.g. the CPU visible one, is only narrowed */
	place = (struct ttm_place) { .lpfn = SZ_4M >> PAGE_SHIFT };
	xe_vram_pool_place(kernel, &place);
	KUNIT_EXPECT_EQ(test, place.fpfn, 0u);
	KUNIT_EXPECT_EQ(test, place.lpfn, SZ_4M >> PAGE_SHIFT);

	place = (struct ttm_place) { .lpfn = SZ_32M >> PAGE_SHIFT };
	xe_vram_pool_place(general, &place);
	KUNIT_EXPECT_EQ(test, place.fpfn, SZ_8M >> PAGE_SHIFT);
	KUNIT_EXPECT_EQ(test, place.lpfn, SZ_32M >> PAGE_SHIFT);
}

static void pool_by_addr(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	struct xe_vram_pool *general = tile_pool(test, xe);
	struct xe_vram_region *vram = general->region;
	struct xe_vram_pool *kernel = &vram->partitions[1];

	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, 0), general);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_64M - SZ_4K), general);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_64M), NULL);

	KUNIT_ASSERT_NOT_NULL(test, xe_vram_pool_create(vram, XE_VRAM_POOL_KERNEL, SZ_8M));

	/* An address resolves to the pool covering it, whatever its purpose */
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, 0), kernel);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_8M - SZ_4K), kernel);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_8M), general);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_64M - SZ_4K), general);
	KUNIT_EXPECT_PTR_EQ(test, xe_vram_region_addr_to_pool(vram, SZ_64M), NULL);
}

static struct kunit_case xe_vram_tests[] = {
	KUNIT_CASE(kernel_pool_defaults_to_general),
	KUNIT_CASE(partition_carve),
	KUNIT_CASE(partition_limit),
	KUNIT_CASE(pool_place),
	KUNIT_CASE(pool_by_addr),
	{}
};

static struct kunit_suite xe_vram_test_suite = {
	.name = "xe_vram",
	.test_cases = xe_vram_tests,
	.init = xe_vram_test_init,
};

kunit_test_suite(xe_vram_test_suite);
