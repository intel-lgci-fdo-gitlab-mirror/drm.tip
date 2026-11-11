/* SPDX-License-Identifier: MIT */
/* Copyright © 2025 Intel Corporation */

#ifndef __DRM_INTEL_GTT_VIEW_TYPES_H__
#define __DRM_INTEL_GTT_VIEW_TYPES_H__

#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/types.h>

struct intel_remapped_plane_info {
	/* in gtt pages */
	u32 offset:31;
	u32 linear:1;
	union {
		/* in gtt pages for !linear */
		struct {
			u16 width;
			u16 height;
			u16 src_stride;
			u16 dst_stride;
		};

		/* in gtt pages for linear */
		u32 size;
	};
} __packed;

struct intel_rotation_info {
	struct intel_remapped_plane_info plane[2];
} __packed;

struct intel_partial_info {
	u64 offset;
	unsigned int size;
} __packed;

struct intel_remapped_info {
	struct intel_remapped_plane_info plane[4];
	/* in gtt pages */
	u32 plane_alignment;
} __packed;

enum intel_gtt_view_type {
	INTEL_GTT_VIEW_NORMAL = 0,
	INTEL_GTT_VIEW_ROTATED = sizeof(struct intel_rotation_info),
	INTEL_GTT_VIEW_PARTIAL = sizeof(struct intel_partial_info),
	INTEL_GTT_VIEW_REMAPPED = sizeof(struct intel_remapped_info),
};

struct intel_gtt_view {
	enum intel_gtt_view_type type;
	union {
		/* Members need to contain no holes/padding */
		struct intel_partial_info partial;
		struct intel_rotation_info rotated;
		struct intel_remapped_info remapped;
	};
};

static inline bool intel_gtt_view_is_normal(const struct intel_gtt_view *view)
{
	return view->type == INTEL_GTT_VIEW_NORMAL;
}

static inline bool intel_gtt_view_is_remapped(const struct intel_gtt_view *view)
{
	return view->type == INTEL_GTT_VIEW_REMAPPED;
}

static inline bool intel_gtt_view_is_rotated(const struct intel_gtt_view *view)
{
	return view->type == INTEL_GTT_VIEW_ROTATED;
}

static inline bool intel_gtt_view_is_partial(const struct intel_gtt_view *view)
{
	return view->type == INTEL_GTT_VIEW_PARTIAL;
}

static inline unsigned int intel_rotation_info_size(const struct intel_rotation_info *rot_info)
{
	unsigned int size = 0;
	int i;

	for (i = 0 ; i < ARRAY_SIZE(rot_info->plane); i++)
		size += rot_info->plane[i].dst_stride * rot_info->plane[i].width;

	return size;
}

static inline unsigned int intel_remapped_info_size(const struct intel_remapped_info *rem_info)
{
	unsigned int size = 0;
	int i;

	for (i = 0 ; i < ARRAY_SIZE(rem_info->plane); i++) {
		unsigned int plane_size;

		if (rem_info->plane[i].linear)
			plane_size = rem_info->plane[i].size;
		else
			plane_size = rem_info->plane[i].dst_stride * rem_info->plane[i].height;

		if (plane_size == 0)
			continue;

		if (rem_info->plane_alignment)
			size = ALIGN(size, rem_info->plane_alignment);

		size += plane_size;
	}

	return size;
}

#endif /* __DRM_INTEL_GTT_VIEW_TYPES_H__ */
