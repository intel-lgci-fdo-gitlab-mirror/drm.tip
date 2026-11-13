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

static void cper_test_pci_ids_have_guid(struct kunit *test)
{
	unsigned int i;

	KUNIT_ASSERT_GT(test, ARRAY_SIZE(cper_test_pci_ids), 0);

	for (i = 0; i < ARRAY_SIZE(cper_test_pci_ids); i++)
		KUNIT_EXPECT_NOT_NULL_MSG(test, cper_test_guid_entry(cper_test_pci_ids[i]),
					  "PCI ID %#06x has no entry in xe_device_guids[]",
					  cper_test_pci_ids[i]);
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

static struct kunit_case xe_cper_test_cases[] = {
	KUNIT_CASE(cper_test_pci_ids_have_guid),
	KUNIT_CASE(cper_test_guids_have_pci_id),
	KUNIT_CASE(cper_test_guids_unique),
	{}
};

static struct kunit_suite xe_cper_suite = {
	.name = "xe_cper",
	.test_cases = xe_cper_test_cases,
};

kunit_test_suites(&xe_cper_suite);
