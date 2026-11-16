// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <kunit/test.h>

#include "xe_vram_types.h"

/* The layout xe_ttm_vram_mgr_init() derives from the pools of a region */
static struct xe_ttm_vram_mgr *split_mgr(struct kunit *test, u64 kernel_size, u64 size)
{
	struct xe_ttm_vram_mgr *mgr;
	struct xe_vram_pool *pools;

	mgr = kunit_kzalloc(test, sizeof(*mgr), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mgr);
	pools = kunit_kcalloc(test, 2, sizeof(*pools), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pools);

	pools[0].offset = 0;
	pools[0].size = kernel_size;
	pools[0].purpose = XE_VRAM_POOL_KERNEL;
	pools[1].offset = kernel_size;
	pools[1].size = size - kernel_size;
	pools[1].purpose = XE_VRAM_POOL_GENERAL;
	mgr->parts[0].pool = &pools[0];
	mgr->parts[1].pool = &pools[1];
	mgr->num_parts = 2;

	return mgr;
}

static void select_part(struct kunit *test)
{
	struct xe_ttm_vram_mgr *mgr = split_mgr(test, SZ_128M, SZ_1G);
	struct xe_ttm_vram_mgr_part *part;
	bool exact = false;

	/* The ranges a pool resolves to cover a whole partition */
	part = xe_ttm_vram_mgr_select_part(mgr, 0, SZ_128M, &exact);
	KUNIT_EXPECT_PTR_EQ(test, part, &mgr->parts[0]);
	KUNIT_EXPECT_TRUE(test, exact);

	part = xe_ttm_vram_mgr_select_part(mgr, SZ_128M, SZ_1G, &exact);
	KUNIT_EXPECT_PTR_EQ(test, part, &mgr->parts[1]);
	KUNIT_EXPECT_TRUE(test, exact);

	/* A narrower range still resolves, but needs a range allocation */
	part = xe_ttm_vram_mgr_select_part(mgr, SZ_256M, SZ_512M, &exact);
	KUNIT_EXPECT_PTR_EQ(test, part, &mgr->parts[1]);
	KUNIT_EXPECT_FALSE(test, exact);

	/* Partitions are a hard split */
	part = xe_ttm_vram_mgr_select_part(mgr, 0, SZ_256M, &exact);
	KUNIT_EXPECT_NULL(test, part);
	part = xe_ttm_vram_mgr_select_part(mgr, 0, SZ_2G, &exact);
	KUNIT_EXPECT_NULL(test, part);
}

static void addr_to_part(struct kunit *test)
{
	struct xe_ttm_vram_mgr *mgr = split_mgr(test, SZ_128M, SZ_1G);

	KUNIT_EXPECT_PTR_EQ(test, xe_ttm_vram_mgr_addr_to_part(mgr, 0), &mgr->parts[0]);
	KUNIT_EXPECT_PTR_EQ(test, xe_ttm_vram_mgr_addr_to_part(mgr, SZ_128M - PAGE_SIZE),
			    &mgr->parts[0]);
	KUNIT_EXPECT_PTR_EQ(test, xe_ttm_vram_mgr_addr_to_part(mgr, SZ_128M), &mgr->parts[1]);
	KUNIT_EXPECT_PTR_EQ(test, xe_ttm_vram_mgr_addr_to_part(mgr, SZ_1G - PAGE_SIZE),
			    &mgr->parts[1]);
	KUNIT_EXPECT_NULL(test, xe_ttm_vram_mgr_addr_to_part(mgr, SZ_1G));
}

static void general_pool_usage(struct kunit *test)
{
	struct xe_ttm_vram_mgr *mgr = split_mgr(test, SZ_128M, SZ_1G);
	LIST_HEAD(kernel_blocks);
	LIST_HEAD(general_blocks);
	u64 kernel_visible = 0;
	u64 general_visible = 0;
	u64 used, used_visible;
	unsigned int i;
	int err;

	mutex_init(&mgr->lock);
	/* Only the bottom 256M are CPU visible */
	mgr->visible_size = SZ_256M;
	mgr->visible_avail = SZ_256M;
	for (i = 0; i < mgr->num_parts; ++i) {
		KUNIT_ASSERT_EQ(test, gpu_buddy_init(&mgr->parts[i].mm,
						     mgr->parts[i].pool->size,
						     PAGE_SIZE), 0);
		gpu_buddy_driver_set_lock(&mgr->parts[i].mm, &mgr->lock);
	}

	mutex_lock(&mgr->lock);
	err = xe_ttm_vram_buddy_alloc(mgr, &mgr->parts[0], 0, SZ_128M, SZ_8M,
				      PAGE_SIZE, &kernel_blocks,
				      GPU_BUDDY_RANGE_ALLOCATION, NULL,
				      &kernel_visible);
	KUNIT_EXPECT_EQ(test, err, 0);
	/* Straddles the end of the CPU visible window */
	err = xe_ttm_vram_buddy_alloc(mgr, &mgr->parts[1], SZ_256M - SZ_4M,
				      SZ_256M + SZ_4M, SZ_8M, PAGE_SIZE,
				      &general_blocks,
				      GPU_BUDDY_RANGE_ALLOCATION, NULL,
				      &general_visible);
	KUNIT_EXPECT_EQ(test, err, 0);
	mutex_unlock(&mgr->lock);

	KUNIT_EXPECT_EQ(test, kernel_visible, (u64)SZ_8M);
	KUNIT_EXPECT_EQ(test, general_visible, (u64)SZ_4M);
	KUNIT_EXPECT_EQ(test, mgr->visible_avail, (u64)(SZ_256M - SZ_8M - SZ_4M));

	/* Memory taken from the kernel-only pool is not reported */
	xe_ttm_vram_get_general_pool_used(&mgr->manager, &used, &used_visible);
	KUNIT_EXPECT_EQ(test, used, (u64)SZ_8M);
	KUNIT_EXPECT_EQ(test, used_visible, (u64)SZ_4M);

	mutex_lock(&mgr->lock);
	xe_ttm_vram_buddy_free(mgr, &mgr->parts[0], &kernel_blocks, kernel_visible);
	xe_ttm_vram_buddy_free(mgr, &mgr->parts[1], &general_blocks, general_visible);
	mutex_unlock(&mgr->lock);

	xe_ttm_vram_get_general_pool_used(&mgr->manager, &used, &used_visible);
	KUNIT_EXPECT_EQ(test, used, 0ull);
	KUNIT_EXPECT_EQ(test, used_visible, 0ull);
	KUNIT_EXPECT_EQ(test, mgr->visible_avail, (u64)SZ_256M);

	for (i = 0; i < mgr->num_parts; ++i)
		gpu_buddy_fini(&mgr->parts[i].mm);
	mutex_destroy(&mgr->lock);
}

static struct kunit_case xe_ttm_vram_mgr_tests[] = {
	KUNIT_CASE(select_part),
	KUNIT_CASE(addr_to_part),
	KUNIT_CASE(general_pool_usage),
	{}
};

static struct kunit_suite xe_ttm_vram_mgr_test_suite = {
	.name = "xe_ttm_vram_mgr",
	.test_cases = xe_ttm_vram_mgr_tests,
};

kunit_test_suite(xe_ttm_vram_mgr_test_suite);
