// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for the drm_atomic_commit helpers
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_probe_helper.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define DRM_TEST_ENC_0 BIT(0)
#define DRM_TEST_ENC_1 BIT(1)
#define DRM_TEST_ENC_2 BIT(2)

#define DRM_TEST_CONN_0 BIT(0)

struct drm_clone_mode_test {
	const char *name;
	u32 encoder_mask;
	int expected_result;
};

static const struct drm_display_mode drm_atomic_test_mode = {
	DRM_MODE("1024x768", 0, 65000, 1024, 1048,
		 1184, 1344, 0, 768, 771, 777, 806, 0,
		 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC)
};

struct drm_atomic_test_priv {
	struct drm_device drm;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoders[3];
	struct drm_connector connectors[2];

	/* for drm_test_fake_commit_flood() */
	atomic_t flood_inflight;
	struct completion flood_drained;
	bool flood_released;
};

static int modeset_counter;

static void drm_test_encoder_mode_set(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	modeset_counter++;
}

static const struct drm_encoder_helper_funcs drm_atomic_test_encoder_funcs = {
	.atomic_mode_set	= drm_test_encoder_mode_set,
};

static const struct drm_connector_funcs dummy_connector_funcs = {
	.atomic_create_state	= drm_atomic_helper_connector_create_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
};

static int drm_atomic_test_dummy_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &drm_atomic_test_mode);
}

static const struct drm_connector_helper_funcs dummy_connector_helper_funcs = {
	.get_modes	= drm_atomic_test_dummy_get_modes,
};

static struct drm_atomic_test_priv *
drm_atomic_test_init_drm_components(struct kunit *test, bool has_connectors)
{
	struct drm_atomic_test_priv *priv;
	struct drm_encoder *enc;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	priv = drm_kunit_helper_alloc_drm_device(test, dev,
						 struct drm_atomic_test_priv,
						 drm,
						 DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	test->priv = priv;

	drm = &priv->drm;
	priv->plane = drm_kunit_helper_create_primary_plane(test, drm,
							    NULL,
							    NULL,
							    NULL, 0,
							    NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->plane);

	priv->crtc = drm_kunit_helper_create_crtc(test, drm,
						  priv->plane, NULL,
						  NULL,
						  NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->crtc);

	for (int i = 0; i < ARRAY_SIZE(priv->encoders); i++) {
		enc = &priv->encoders[i];

		ret = drmm_encoder_init(drm, enc, NULL,
					DRM_MODE_ENCODER_DSI, NULL);
		KUNIT_ASSERT_EQ(test, ret, 0);

		enc->possible_crtcs = drm_crtc_mask(priv->crtc);
	}

	priv->encoders[0].possible_clones = DRM_TEST_ENC_0 | DRM_TEST_ENC_1;
	priv->encoders[1].possible_clones = DRM_TEST_ENC_0 | DRM_TEST_ENC_1;
	priv->encoders[2].possible_clones = DRM_TEST_ENC_2;

	if (!has_connectors)
		goto done;

	BUILD_BUG_ON(ARRAY_SIZE(priv->connectors) > ARRAY_SIZE(priv->encoders));

	for (int i = 0; i < ARRAY_SIZE(priv->connectors); i++) {
		conn = &priv->connectors[i];

		ret = drmm_connector_init(drm, conn, &dummy_connector_funcs,
					  DRM_MODE_CONNECTOR_DSI, NULL);
		KUNIT_ASSERT_EQ(test, ret, 0);

		drm_connector_helper_add(conn, &dummy_connector_helper_funcs);
		drm_encoder_helper_add(&priv->encoders[i],
				       &drm_atomic_test_encoder_funcs);

		drm_connector_attach_encoder(conn, &priv->encoders[i]);
	}

done:
	drm_mode_config_reset(drm);

	return priv;
}

static int set_up_atomic_state(struct kunit *test,
			       struct drm_atomic_test_priv *priv,
			       struct drm_connector *connector,
			       struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *drm = &priv->drm;
	struct drm_crtc *crtc = priv->crtc;
	struct drm_atomic_commit *state;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	int ret;

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	if (connector) {
		conn_state = drm_atomic_get_connector_state(state, connector);
		if (IS_ERR(conn_state))
			return PTR_ERR(conn_state);

		ret = drm_atomic_set_crtc_for_connector(conn_state, crtc);
		if (ret)
			return ret;
	}

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, &drm_atomic_test_mode);
	if (ret)
		return ret;

	crtc_state->enable = true;
	crtc_state->active = true;

	if (connector) {
		ret = drm_atomic_commit(state);
		if (ret)
			return ret;
	} else {
		// dummy connector mask
		crtc_state->connector_mask = DRM_TEST_CONN_0;
	}

	return 0;
}

/*
 * Test that the DRM encoder mode_set() is called when the atomic state
 * connectors are changed but the CRTC mode is not.
 */
static void drm_test_check_connector_changed_modeset(struct kunit *test)
{
	struct drm_atomic_test_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_connector *old_conn, *new_conn;
	struct drm_atomic_commit *state;
	struct drm_device *drm;
	struct drm_connector_state *new_conn_state, *old_conn_state;
	int ret, initial_modeset_count;

	priv = drm_atomic_test_init_drm_components(test, true);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;
	old_conn = &priv->connectors[0];
	new_conn = &priv->connectors[1];

	drm_modeset_acquire_init(&ctx, 0);

	// first modeset to enable
retry_set_up:
	ret = set_up_atomic_state(test, priv, old_conn, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_set_up;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, new_conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_connector_state(state, old_conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	ret = drm_atomic_set_crtc_for_connector(old_conn_state, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_atomic_set_crtc_for_connector(new_conn_state, priv->crtc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	initial_modeset_count = modeset_counter;

	// modeset_disables is called as part of the atomic commit tail
	ret = drm_atomic_commit(state);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, modeset_counter, initial_modeset_count + 1);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/*
 * Test that the drm_crtc_in_clone_mode() helper can detect if a given CRTC
 * state is in clone mode
 */
static void drm_test_check_in_clone_mode(struct kunit *test)
{
	bool ret;
	const struct drm_clone_mode_test *param = test->param_value;
	struct drm_crtc_state *crtc_state;

	crtc_state = kunit_kzalloc(test, sizeof(*crtc_state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, crtc_state);

	crtc_state->encoder_mask = param->encoder_mask;

	ret = drm_crtc_in_clone_mode(crtc_state);

	KUNIT_ASSERT_EQ(test, ret, param->expected_result);
}

/*
 * Test that the atomic commit path will succeed for valid clones (or non-cloned
 * states) and fail for states where the cloned encoders are not possible_clones
 * of each other.
 */
static void drm_test_check_valid_clones(struct kunit *test)
{
	int ret;
	const struct drm_clone_mode_test *param = test->param_value;
	struct drm_atomic_test_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_device *drm;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;

	priv = drm_atomic_test_init_drm_components(test, false);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm = &priv->drm;

	drm_modeset_acquire_init(&ctx, 0);

retry_set_up:
	ret = set_up_atomic_state(test, priv, NULL, &ctx);
	if (ret == -EDEADLK) {
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry_set_up;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

retry:
	crtc_state = drm_atomic_get_crtc_state(state, priv->crtc);
	if (PTR_ERR(crtc_state) == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	crtc_state->encoder_mask = param->encoder_mask;

	// force modeset
	crtc_state->mode_changed = true;

	ret = drm_atomic_helper_check_modeset(drm, state);
	if (ret == -EDEADLK) {
		drm_atomic_commit_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
	KUNIT_ASSERT_EQ(test, ret, param->expected_result);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static void drm_check_in_clone_mode_desc(const struct drm_clone_mode_test *t,
				      char *desc)
{
	sprintf(desc, "%s", t->name);
}

static void drm_check_valid_clones_desc(const struct drm_clone_mode_test *t,
				      char *desc)
{
	sprintf(desc, "%s", t->name);
}

static const struct drm_clone_mode_test drm_clone_mode_tests[] = {
	{
		.name = "in_clone_mode",
		.encoder_mask = DRM_TEST_ENC_0 | DRM_TEST_ENC_1,
		.expected_result = true,
	},
	{
		.name = "not_in_clone_mode",
		.encoder_mask = DRM_TEST_ENC_0,
		.expected_result = false,
	},
};

static const struct drm_clone_mode_test drm_valid_clone_mode_tests[] = {
	{
		.name = "not_in_clone_mode",
		.encoder_mask = DRM_TEST_ENC_0,
		.expected_result = 0,
	},

	{
		.name = "valid_clone",
		.encoder_mask = DRM_TEST_ENC_0 | DRM_TEST_ENC_1,
		.expected_result = 0,
	},
	{
		.name = "invalid_clone",
		.encoder_mask = DRM_TEST_ENC_0 | DRM_TEST_ENC_2,
		.expected_result = -EINVAL,
	},
};

KUNIT_ARRAY_PARAM(drm_check_in_clone_mode, drm_clone_mode_tests,
		  drm_check_in_clone_mode_desc);

KUNIT_ARRAY_PARAM(drm_check_valid_clones, drm_valid_clone_mode_tests,
		  drm_check_valid_clones_desc);

static struct kunit_case drm_test_check_modeset_test[] = {
	KUNIT_CASE(drm_test_check_connector_changed_modeset),
	{}
};

static struct kunit_case drm_in_clone_mode_check_test[] = {
	KUNIT_CASE_PARAM(drm_test_check_in_clone_mode,
			 drm_check_in_clone_mode_gen_params),
	KUNIT_CASE_PARAM(drm_test_check_valid_clones,
			 drm_check_valid_clones_gen_params),
	{}
};

/*
 * Test that the commit used for planes which are not bound to any CRTC has
 * flip_done completed already by drm_atomic_helper_setup_commit(), since there
 * is no flip to wait for, while hw_done is left for the commit to complete.
 */
static void drm_test_fake_commit_flip_done(struct kunit *test)
{
	struct drm_atomic_test_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_plane_state *plane_state;
	int ret;

	priv = drm_atomic_test_init_drm_components(test, false);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	plane_state = drm_atomic_get_plane_state(state, priv->plane);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, plane_state);
	KUNIT_ASSERT_NULL(test, plane_state->crtc);
	KUNIT_ASSERT_NULL(test, priv->plane->state->crtc);

	ret = drm_atomic_helper_setup_commit(state, false);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, state->fake_commit);
	KUNIT_EXPECT_PTR_EQ(test, plane_state->commit, state->fake_commit);
	KUNIT_EXPECT_PTR_EQ(test, state->fake_commit->dev, &priv->drm);
	KUNIT_EXPECT_NULL(test, state->fake_commit->crtc);
	KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&state->fake_commit->flip_done));
	KUNIT_EXPECT_FALSE(test, try_wait_for_completion(&state->fake_commit->hw_done));

	drm_atomic_commit_clear(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

struct drm_test_hw_done_work {
	struct delayed_work work;
	struct drm_crtc_commit *commit;
};

static void drm_test_hw_done_work_func(struct work_struct *work)
{
	struct drm_test_hw_done_work *w =
		container_of(work, struct drm_test_hw_done_work, work.work);

	complete_all(&w->commit->hw_done);
}

/*
 * Test that a nonblocking commit does not fail with -EBUSY just because the
 * previous commit that touched the same unbound plane is still in flight, ie.
 * its hw_done is yet to complete. It should instead wait for the hw_done, so
 * that userspace can not get arbitrarily far ahead of the commit work.
 */
static void drm_test_fake_commit_not_busy(struct kunit *test)
{
	struct drm_atomic_test_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state, *prev;
	struct drm_test_hw_done_work w;
	int ret;

	priv = drm_atomic_test_init_drm_components(test, false);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	/* The previous commit, which became the plane's current state */
	prev = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm_atomic_get_plane_state(prev, priv->plane));
	ret = drm_atomic_helper_setup_commit(prev, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, prev->fake_commit);
	priv->plane->state->commit = drm_crtc_commit_get(prev->fake_commit);

	state = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm_atomic_get_plane_state(state, priv->plane));

	w.commit = prev->fake_commit;
	INIT_DELAYED_WORK_ONSTACK(&w.work, drm_test_hw_done_work_func);
	schedule_delayed_work(&w.work, msecs_to_jiffies(20));

	ret = drm_atomic_helper_setup_commit(state, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&prev->fake_commit->hw_done));

	flush_delayed_work(&w.work);
	destroy_delayed_work_on_stack(&w.work);

	drm_atomic_commit_clear(state);
	drm_atomic_commit_clear(prev);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static void drm_test_flood_commit_work(struct work_struct *work)
{
	struct drm_atomic_commit *state =
		container_of(work, struct drm_atomic_commit, commit_work);
	struct drm_atomic_test_priv *priv =
		container_of(state->dev, struct drm_atomic_test_priv, drm);

	drm_atomic_helper_wait_for_dependencies(state);

	/* Pretend to do some work */
	msleep(2);

	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_commit_cleanup_done(state);

	/* The commit must be gone before the test can tear down the device */
	drm_atomic_commit_put(state);

	if (atomic_dec_and_test(&priv->flood_inflight))
		complete_all(&priv->flood_drained);
}

/*
 * Wait for all the commit work items to finish. The submitter holds one
 * reference to flood_inflight until it is done submitting, so that the
 * completion can not fire early. Also used as a cleanup action, to make sure
 * no work item outlives the test if it bails out.
 */
static void drm_test_flood_drain(void *data)
{
	struct drm_atomic_test_priv *priv = data;

	if (!xchg(&priv->flood_released, true) &&
	    atomic_dec_and_test(&priv->flood_inflight))
		complete_all(&priv->flood_drained);

	wait_for_completion(&priv->flood_drained);
}

static int drm_test_flood_submit(struct kunit *test,
				 struct drm_atomic_test_priv *priv)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_commit *prev;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_commit_alloc(&priv->drm);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;

	plane_state = drm_atomic_get_plane_state(state, priv->plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto err_put;
	}

	/* Stable, the plane is locked */
	prev = priv->plane->state->commit;

	ret = drm_atomic_helper_setup_commit(state, true);
	if (ret)
		goto err_put;

	/*
	 * Userspace must not get ahead of the previous commit's hw_done,
	 * whatever the commit work is up to.
	 */
	if (prev)
		KUNIT_EXPECT_TRUE(test, try_wait_for_completion(&prev->hw_done));

	ret = drm_atomic_helper_swap_state(state, false);
	if (ret)
		goto err_put;

	/* The ctx goes away, but the state lives on in the work item */
	state->acquire_ctx = NULL;

	atomic_inc(&priv->flood_inflight);
	INIT_WORK(&state->commit_work, drm_test_flood_commit_work);
	queue_work(system_dfl_wq, &state->commit_work);

	ret = 0;
	goto out;

err_put:
	drm_atomic_commit_put(state);
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

/*
 * Test that a flood of nonblocking commits that only touch an unbound plane
 * can not queue up an unbounded number of commits, even if the driver does not
 * ask swap_state() to stall. Otherwise the work items would all wait for the
 * hw_done of their predecessor, and the waits would add up.
 */
static void drm_test_fake_commit_flood(struct kunit *test)
{
	struct drm_atomic_test_priv *priv;
	int i, ret;

	priv = drm_atomic_test_init_drm_components(test, false);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	atomic_set(&priv->flood_inflight, 1);
	init_completion(&priv->flood_drained);

	ret = kunit_add_action_or_reset(test, drm_test_flood_drain, priv);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for (i = 0; i < 100; i++) {
		ret = drm_test_flood_submit(test, priv);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	drm_test_flood_drain(priv);
}

/*
 * Test that the commits know their device, and that only the one for a CRTC
 * knows a CRTC, so that the waits can report timeouts for either.
 */
static void drm_test_commit_dev(struct kunit *test)
{
	struct drm_atomic_test_priv *priv;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;
	int ret;

	priv = drm_atomic_test_init_drm_components(test, false);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, &priv->drm, &ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	crtc_state = drm_atomic_get_crtc_state(state, priv->crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_helper_setup_commit(state, false);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL(test, crtc_state->commit);
	KUNIT_EXPECT_PTR_EQ(test, crtc_state->commit->dev, &priv->drm);
	KUNIT_EXPECT_PTR_EQ(test, crtc_state->commit->crtc, priv->crtc);

	drm_atomic_commit_clear(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

static struct kunit_case drm_fake_commit_tests[] = {
	KUNIT_CASE(drm_test_fake_commit_flip_done),
	KUNIT_CASE(drm_test_fake_commit_not_busy),
	KUNIT_CASE(drm_test_fake_commit_flood),
	KUNIT_CASE(drm_test_commit_dev),
	{}
};

static struct kunit_suite drm_fake_commit_test_suite = {
	.name = "drm_atomic_fake_commit",
	.test_cases = drm_fake_commit_tests,
};

static struct kunit_suite drm_test_check_modeset_test_suite = {
	.name = "drm_validate_modeset",
	.test_cases = drm_test_check_modeset_test,
};

static struct kunit_suite drm_in_clone_mode_check_test_suite = {
	.name = "drm_validate_clone_mode",
	.test_cases = drm_in_clone_mode_check_test,
};

kunit_test_suites(&drm_in_clone_mode_check_test_suite,
		  &drm_test_check_modeset_test_suite,
		  &drm_fake_commit_test_suite);

MODULE_AUTHOR("Jessica Zhang <quic_jesszhan@quicinc.com");
MODULE_DESCRIPTION("Test cases for the drm_atomic_helper functions");
MODULE_LICENSE("GPL");
