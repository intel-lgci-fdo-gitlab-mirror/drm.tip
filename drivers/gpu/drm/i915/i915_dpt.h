/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Intel Corporation */

#ifndef __I915_DPT_H__
#define __I915_DPT_H__

#include <linux/types.h>

struct drm_i915_gem_object;
struct i915_address_space;
struct i915_ggtt;
struct i915_vma;
struct intel_dpt;
struct intel_gtt_view;

struct intel_dpt *i915_vm_to_dpt(struct i915_address_space *vm);
void i915_dpt_obj_free(struct drm_i915_gem_object *obj);
struct intel_dpt *i915_dpt_lookup(struct drm_i915_gem_object *obj,
				  const struct intel_gtt_view *view);
struct intel_dpt *i915_dpt_instance(struct drm_i915_gem_object *obj,
				    const struct intel_gtt_view *view);

struct i915_address_space *i915_dpt_to_vm(struct intel_dpt *dpt);
struct i915_vma *i915_dpt_pin_to_ggtt(struct intel_dpt *dpt, unsigned int alignment);
void i915_dpt_unpin_from_ggtt(struct intel_dpt *dpt);
u64 i915_dpt_offset(struct i915_vma *dpt_vma);

void i915_dpt_suspend(struct i915_ggtt *ggtt);
void i915_dpt_resume(struct i915_ggtt *ggtt);

#endif /* __I915_DPT_H__ */
