// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <kunit/test.h>

#include <drm/intel/pciids.h>

/*
 * PCI IDs for platforms that support CPER records.
 * Must stay in sync with the xe PCI driver table.
 */
#define CPER_TEST_PCI_ID(id) (id)
static const u16 cper_test_pci_ids[] = {
	INTEL_CRI_IDS(CPER_TEST_PCI_ID),
};

#undef CPER_TEST_PCI_ID

/* UUIDv5 outputs for "platform/8086:<device_id>". */
static const struct xe_device_guid_entry cper_test_expected_guids[] = {
	{ 0x674C, GUID_INIT(0x9046afe5, 0x9041, 0x5124,
			    0x86, 0x14, 0x92, 0x55, 0x0d, 0x9e, 0x9d, 0xa6) },
	{ 0x674D, GUID_INIT(0xf95911b8, 0x6564, 0x5905,
			    0xa6, 0xa7, 0xb9, 0xbf, 0x54, 0xab, 0x6c, 0x27) },
	{ 0x674E, GUID_INIT(0xb8d24106, 0x0108, 0x59f1,
			    0x8f, 0xfc, 0x9f, 0xc4, 0xa9, 0xc9, 0xb7, 0xb0) },
	{ 0x674F, GUID_INIT(0xdd8dd88e, 0xef2f, 0x59b1,
			    0x99, 0x15, 0xdb, 0x6f, 0x1d, 0xc9, 0xb4, 0x5d) },
	{ 0x6750, GUID_INIT(0x1dd66c44, 0x87d5, 0x524a,
			    0xad, 0x6e, 0xfc, 0x18, 0x9d, 0x42, 0x6f, 0x1a) },
	{ 0x6751, GUID_INIT(0x17d0dd04, 0x2fad, 0x5400,
			    0x99, 0xc8, 0x9b, 0x45, 0x51, 0x3b, 0x67, 0xe3) },
	{ 0x6752, GUID_INIT(0xb778ead8, 0x0009, 0x59a7,
			    0xa1, 0x3a, 0x78, 0xce, 0x4d, 0xdd, 0xae, 0xf5) },
};

static bool cper_test_pci_id_known(u16 device_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cper_test_pci_ids); i++)
		if (cper_test_pci_ids[i] == device_id)
			return true;

	return false;
}

static const struct xe_device_guid_entry *cper_test_guid_entry(u16 device_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xe_device_guids); i++)
		if (xe_device_guids[i].device_id == device_id)
			return &xe_device_guids[i];

	return NULL;
}

static const struct xe_device_guid_entry *cper_test_expected_guid_entry(u16 device_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cper_test_expected_guids); i++)
		if (cper_test_expected_guids[i].device_id == device_id)
			return &cper_test_expected_guids[i];

	return NULL;
}

static void cper_test_pci_ids_have_guid(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_GT(test, ARRAY_SIZE(cper_test_pci_ids), 0);

	for (i = 0; i < ARRAY_SIZE(cper_test_pci_ids); i++)
		KUNIT_EXPECT_NOT_NULL_MSG(test, cper_test_guid_entry(cper_test_pci_ids[i]),
					  "PCI ID %#06x has no entry in xe_device_guids[]",
					  cper_test_pci_ids[i]);
}

static void cper_test_guid_matches_pci_id(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_EQ(test, ARRAY_SIZE(cper_test_expected_guids),
			ARRAY_SIZE(cper_test_pci_ids));

	for (i = 0; i < ARRAY_SIZE(cper_test_pci_ids); i++) {
		u16 device_id = cper_test_pci_ids[i];
		const struct xe_device_guid_entry *actual = cper_test_guid_entry(device_id);
		const struct xe_device_guid_entry *expected =
			cper_test_expected_guid_entry(device_id);

		KUNIT_ASSERT_NOT_NULL_MSG(test, expected,
					  "PCI ID %#06x has no expected GUID",
					  device_id);
		KUNIT_ASSERT_NOT_NULL_MSG(test, actual,
					  "PCI ID %#06x has no GUID entry",
					  device_id);
		KUNIT_EXPECT_TRUE_MSG(test, guid_equal(&actual->guid, &expected->guid),
				      "PCI ID %#06x has GUID %pUl, expected %pUl",
				      device_id, &actual->guid, &expected->guid);
	}
}

static void cper_test_guids_have_pci_id(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xe_device_guids); i++)
		KUNIT_EXPECT_TRUE_MSG(test, cper_test_pci_id_known(xe_device_guids[i].device_id),
				      "xe_device_guids[%u] PCI ID %#06x is not a known platform PCI ID",
				      i, xe_device_guids[i].device_id);
}

static void cper_test_guids_unique(struct kunit *test)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(xe_device_guids); i++) {
		for (j = i + 1; j < ARRAY_SIZE(xe_device_guids); j++) {
			KUNIT_EXPECT_NE_MSG(test, xe_device_guids[i].device_id,
					    xe_device_guids[j].device_id,
					    "duplicate PCI ID %#06x at [%u] and [%u]",
					    xe_device_guids[i].device_id, i, j);
			KUNIT_EXPECT_FALSE_MSG(test, guid_equal(&xe_device_guids[i].guid,
								&xe_device_guids[j].guid),
					       "duplicate GUID %pUl for PCI IDs %#06x and %#06x",
					       &xe_device_guids[i].guid,
					       xe_device_guids[i].device_id,
					       xe_device_guids[j].device_id);
		}
	}
}

/*
 * GUIDs are UUIDv5 (RFC 9562): version in byte 7, variant in byte 8.
 */
static void cper_test_guids_are_uuidv5(struct kunit *test)
{
	static const guid_t reserved[] = {
		INTEL_CPER_CREATOR_XEKMD,
		INTEL_CPER_NOTIFY_GPU_ERROR,
		INTEL_CPER_SECTION_ACCEL_GENERIC,
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(xe_device_guids); i++) {
		const struct xe_device_guid_entry *e = &xe_device_guids[i];

		KUNIT_EXPECT_FALSE_MSG(test, guid_is_null(&e->guid),
				       "PCI ID %#06x has a null GUID", e->device_id);
		KUNIT_EXPECT_EQ_MSG(test, e->guid.b[7] >> 4, 5,
				    "PCI ID %#06x GUID %pUl is not UUIDv5",
				    e->device_id, &e->guid);
		KUNIT_EXPECT_EQ_MSG(test, e->guid.b[8] & 0xc0, 0x80,
				    "PCI ID %#06x GUID %pUl has wrong variant",
				    e->device_id, &e->guid);

		for (j = 0; j < ARRAY_SIZE(reserved); j++)
			KUNIT_EXPECT_FALSE_MSG(test, guid_equal(&e->guid, &reserved[j]),
					       "PCI ID %#06x GUID %pUl collides with a CPER type GUID",
					       e->device_id, &e->guid);
	}
}

static struct kunit_case xe_cper_test_cases[] = {
	KUNIT_CASE(cper_test_pci_ids_have_guid),
	KUNIT_CASE(cper_test_guid_matches_pci_id),
	KUNIT_CASE(cper_test_guids_have_pci_id),
	KUNIT_CASE(cper_test_guids_unique),
	KUNIT_CASE(cper_test_guids_are_uuidv5),
	{}
};

static struct kunit_suite xe_cper_suite = {
	.name = "xe_cper",
	.test_cases = xe_cper_test_cases,
};

kunit_test_suites(&xe_cper_suite);
