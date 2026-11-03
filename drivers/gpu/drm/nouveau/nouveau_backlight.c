/*
 * Copyright (C) 2009 Red Hat <mjg@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Authors:
 *  Matthew Garrett <mjg@redhat.com>
 *
 * Register locations derived from NVClock by Roderick Colenbrander
 */

#include <linux/apple-gmux.h>
#include <linux/backlight.h>
#include <linux/idr.h>
#include <drm/drm_probe_helper.h>

#include "nouveau_drv.h"
#include "nouveau_reg.h"
#include "nouveau_encoder.h"
#include "nouveau_connector.h"
#include "nouveau_acpi.h"

static struct ida bl_ida;
#define BL_NAME_SIZE 24 // 12 for name + 11 for digits + 1 for '\0'

static bool
nouveau_get_backlight_name(char backlight_name[BL_NAME_SIZE],
			   struct nouveau_backlight *bl)
{
	const int nb = ida_alloc_max(&bl_ida, 99, GFP_KERNEL);

	if (nb < 0)
		return false;
	if (nb > 0)
		snprintf(backlight_name, BL_NAME_SIZE, "nv_backlight%d", nb);
	else
		snprintf(backlight_name, BL_NAME_SIZE, "nv_backlight");
	bl->id = nb;
	return true;
}

static int
nv40_get_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int val = (nvif_rd32(device, NV40_PMC_BACKLIGHT) &
		   NV40_PMC_BACKLIGHT_MASK) >> 16;

	return val;
}

static int
nv40_set_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int val = bd->props.brightness;
	int reg = nvif_rd32(device, NV40_PMC_BACKLIGHT);

	nvif_wr32(device, NV40_PMC_BACKLIGHT,
		  (val << 16) | (reg & ~NV40_PMC_BACKLIGHT_MASK));

	return 0;
}

static const struct backlight_ops nv40_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = nv40_get_intensity,
	.update_status = nv40_set_intensity,
};

static int
nv40_backlight_init(struct nouveau_encoder *encoder,
		    struct backlight_properties *props,
		    const struct backlight_ops **ops)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;

	if (!(nvif_rd32(device, NV40_PMC_BACKLIGHT) & NV40_PMC_BACKLIGHT_MASK))
		return -ENODEV;

	props->max_brightness = 31;
	*ops = &nv40_bl_ops;
	return 0;
}

/*
 * eDP brightness callbacks need to happen under lock, since we need to
 * enable/disable the backlight ourselves for modesets
 */
static int
nv50_edp_get_brightness(struct backlight_device *bd)
{
	struct drm_connector *connector = dev_get_drvdata(bd->dev.parent);
	struct drm_device *dev = connector->dev;
	struct drm_crtc *crtc;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0;

	drm_modeset_acquire_init(&ctx, 0);

retry:
	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, &ctx);
	if (ret == -EDEADLK)
		goto deadlock;
	else if (ret < 0)
		goto out;

	crtc = connector->state->crtc;
	if (!crtc)
		goto out;

	ret = drm_modeset_lock(&crtc->mutex, &ctx);
	if (ret == -EDEADLK)
		goto deadlock;
	else if (ret < 0)
		goto out;

	if (!crtc->state->active)
		goto out;

	ret = bd->props.brightness;
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
deadlock:
	drm_modeset_backoff(&ctx);
	goto retry;
}

static int
nv50_edp_set_brightness(struct backlight_device *bd)
{
	struct drm_connector *connector = dev_get_drvdata(bd->dev.parent);
	struct nouveau_connector *nv_connector = nouveau_connector(connector);
	struct drm_device *dev = connector->dev;
	struct drm_crtc *crtc;
	struct drm_dp_aux *aux = &nv_connector->aux;
	struct nouveau_backlight *nv_bl = nv_connector->backlight;
	struct drm_modeset_acquire_ctx ctx;
	int ret = 0;

	drm_modeset_acquire_init(&ctx, 0);
retry:
	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, &ctx);
	if (ret == -EDEADLK)
		goto deadlock;
	else if (ret < 0)
		goto out;

	crtc = connector->state->crtc;
	if (!crtc)
		goto out;

	ret = drm_modeset_lock(&crtc->mutex, &ctx);
	if (ret == -EDEADLK)
		goto deadlock;
	else if (ret < 0)
		goto out;

	if (crtc->state->active)
		ret = drm_edp_backlight_set_level(aux, &nv_bl->edp_info, bd->props.brightness);

out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
deadlock:
	drm_modeset_backoff(&ctx);
	goto retry;
}

static const struct backlight_ops nv50_edp_bl_ops = {
	.get_brightness = nv50_edp_get_brightness,
	.update_status = nv50_edp_set_brightness,
};

static int
nv50_get_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);

	return nvif_outp_bl_get(&nv_encoder->outp);
}

static int
nv50_set_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);

	return nvif_outp_bl_set(&nv_encoder->outp, backlight_get_brightness(bd));
}

static const struct backlight_ops nv50_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = nv50_get_intensity,
	.update_status = nv50_set_intensity,
};

/* FIXME: perform backlight probing for eDP _before_ this, this only gets called after connector
 * registration which happens after the initial modeset
 */
static int
nv50_backlight_init(struct nouveau_backlight *bl,
		    struct nouveau_connector *nv_conn,
		    struct nouveau_encoder *nv_encoder,
		    struct backlight_properties *props,
		    const struct backlight_ops **ops)
{
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	int brightness;

	brightness = nvif_outp_bl_get(&nv_encoder->outp);

	/*
	 * Note when this runs the connectors have not been probed yet,
	 * so nv_conn->base.status is not set yet.
	 */
	if (brightness < 0 ||
	    drm_helper_probe_detect(&nv_conn->base, NULL, false) != connector_status_connected)
		return -ENODEV;

	if (nv_conn->type == DCB_CONNECTOR_eDP) {
		int ret;
		u32 current_level;
		u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE];
		u8 current_mode;

		ret = drm_dp_dpcd_read(&nv_conn->aux, DP_EDP_DPCD_REV, edp_dpcd,
				       EDP_DISPLAY_CTL_CAP_SIZE);
		if (ret < 0)
			return ret;

		/* TODO: Add support for hybrid PWM/DPCD panels */
		if (drm_edp_backlight_supported(edp_dpcd) &&
		    (edp_dpcd[1] & DP_EDP_BACKLIGHT_AUX_ENABLE_CAP) &&
		    (edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP)) {
			NV_DEBUG(drm, "DPCD backlight controls supported on %s\n",
				 nv_conn->base.name);

			ret = drm_edp_backlight_init(&nv_conn->aux, &bl->edp_info,
						     0, 0, edp_dpcd,
						     &current_level, &current_mode, false);
			if (ret < 0)
				return ret;

			ret = drm_edp_backlight_enable(&nv_conn->aux, &bl->edp_info, current_level);
			if (ret < 0) {
				NV_ERROR(drm, "Failed to enable backlight on %s: %d\n",
					 nv_conn->base.name, ret);
				return ret;
			}

			*ops = &nv50_edp_bl_ops;
			props->brightness = current_level;
			props->max_brightness = bl->edp_info.max;
			bl->uses_dpcd = true;
			return 0;
		}
	}

	*ops = &nv50_bl_ops;
	props->max_brightness = 100;
	props->brightness = brightness;
	return 0;
}

/*
 * LUMINANCE is an atomic connector property. Pre-NV50 hardware and
 * nouveau.atomic=0 don't expose atomic modesetting to userspace (even though
 * NV50+ still commits atomically internally), so keep the backlight class
 * device there.
 */
static bool
nouveau_backlight_use_luminance(struct drm_device *dev)
{
	return IS_ENABLED(CONFIG_DRM_LUMINANCE) &&
	       drm_core_check_feature(dev, DRIVER_ATOMIC);
}

static int
nouveau_backlight_create(struct drm_connector *connector)
{
	struct nouveau_drm *drm = nouveau_drm(connector->dev);
	struct nouveau_backlight *bl;
	struct nouveau_encoder *nv_encoder = NULL;
	struct nvif_device *device = &drm->client.device;
	char backlight_name[BL_NAME_SIZE];
	struct backlight_properties props = {0};
	const struct backlight_ops *ops;
	int ret;

	if (apple_gmux_present()) {
		NV_INFO_ONCE(drm, "Apple GMUX detected: not registering Nouveau backlight interface\n");
		return 0;
	}

	if (connector->connector_type == DRM_MODE_CONNECTOR_LVDS)
		nv_encoder = find_encoder(connector, DCB_OUTPUT_LVDS);
	else if (connector->connector_type == DRM_MODE_CONNECTOR_eDP)
		nv_encoder = find_encoder(connector, DCB_OUTPUT_DP);
	else
		return 0;

	if (!nv_encoder)
		return 0;

	bl = kzalloc_obj(*bl);
	if (!bl)
		return -ENOMEM;
	bl->id = -1;
	bl->encoder = nv_encoder;

	switch (device->info.family) {
	case NV_DEVICE_INFO_V0_CURIE:
		ret = nv40_backlight_init(nv_encoder, &props, &ops);
		break;
	case NV_DEVICE_INFO_V0_TESLA:
	case NV_DEVICE_INFO_V0_FERMI:
	case NV_DEVICE_INFO_V0_KEPLER:
	case NV_DEVICE_INFO_V0_MAXWELL:
	case NV_DEVICE_INFO_V0_PASCAL:
	case NV_DEVICE_INFO_V0_VOLTA:
	case NV_DEVICE_INFO_V0_TURING:
	case NV_DEVICE_INFO_V0_AMPERE: //XXX: not confirmed
		ret = nv50_backlight_init(bl, nouveau_connector(connector),
					  nv_encoder, &props, &ops);
		break;
	default:
		ret = 0;
		goto fail_alloc;
	}

	if (ret) {
		if (ret == -ENODEV)
			ret = 0;
		goto fail_alloc;
	}

	if (!nouveau_acpi_video_backlight_use_native()) {
		NV_INFO(drm, "Skipping nv_backlight registration\n");
		goto fail_alloc;
	}

	props.type = BACKLIGHT_RAW;
	if (nouveau_backlight_use_luminance(connector->dev)) {
		ret = drm_connector_attach_luminance_property(connector, 0, props.max_brightness,
							      props.brightness);
		if (ret)
			goto fail_alloc;
	} else {
		if (!nouveau_get_backlight_name(backlight_name, bl)) {
			NV_ERROR(drm,
				 "Failed to retrieve a unique name for the backlight interface\n");
			goto fail_alloc;
		}

		bl->dev = backlight_device_register(backlight_name, connector->kdev,
						    nv_encoder, ops, &props);
		if (IS_ERR(bl->dev)) {
			if (bl->id >= 0)
				ida_free(&bl_ida, bl->id);
			ret = PTR_ERR(bl->dev);
			goto fail_alloc;
		}
	}

	nouveau_connector(connector)->backlight = bl;
	if (bl->dev) {
		if (!bl->dev->props.brightness)
			bl->dev->props.brightness =
				bl->dev->ops->get_brightness(bl->dev);
		backlight_update_status(bl->dev);
	}

	return 0;

fail_alloc:
	kfree(bl);
	/*
	 * If we get here we have an internal panel, but no nv_backlight,
	 * try registering an ACPI video backlight device instead.
	 */
	if (ret == 0)
		nouveau_acpi_video_register_backlight();

	return ret;
}

/* Called from .late_register, the backlight class device needs connector->kdev */
int
nouveau_backlight_init(struct drm_connector *connector)
{
	if (nouveau_backlight_use_luminance(connector->dev))
		return 0;

	return nouveau_backlight_create(connector);
}

/* Called before drm_dev_register(), the LUMINANCE property must exist by then */
int
nouveau_backlight_luminance_init(struct drm_device *dev)
{
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	int ret = 0;

	if (!nouveau_backlight_use_luminance(dev))
		return 0;

	drm_connector_list_iter_begin(dev, &conn_iter);
	nouveau_for_each_non_mst_connector_iter(connector, &conn_iter) {
		ret = nouveau_backlight_create(connector);
		if (ret)
			break;
	}
	drm_connector_list_iter_end(&conn_iter);

	return ret;
}

static void
nouveau_backlight_free(struct drm_connector *connector)
{
	struct nouveau_connector *nv_conn = nouveau_connector(connector);
	struct nouveau_backlight *bl = nv_conn->backlight;

	if (!bl)
		return;

	if (bl->id >= 0)
		ida_free(&bl_ida, bl->id);

	backlight_device_unregister(bl->dev);
	nv_conn->backlight = NULL;
	kfree(bl);
}

/* Called from .early_unregister */
void
nouveau_backlight_fini(struct drm_connector *connector)
{
	/* LUMINANCE state is needed by commits until the connector is destroyed */
	if (nouveau_backlight_use_luminance(connector->dev))
		return;

	nouveau_backlight_free(connector);
}

void
nouveau_backlight_destroy(struct drm_connector *connector)
{
	nouveau_backlight_free(connector);
}

void
nouveau_backlight_ctor(void)
{
	ida_init(&bl_ida);
}

void
nouveau_backlight_dtor(void)
{
	ida_destroy(&bl_ida);
}

void
nouveau_backlight_set_luminance(const struct drm_connector_state *conn_state)
{
	struct nouveau_connector *nv_connector = nouveau_connector(conn_state->connector);
	struct nouveau_backlight *bl = nv_connector->backlight;
	unsigned int luminance = conn_state->luminance;

	if (!bl)
		return;

	if (bl->uses_dpcd)
		drm_edp_backlight_set_level(&nv_connector->aux, &bl->edp_info,
					    luminance);
	else
		nvif_outp_bl_set(&bl->encoder->outp, luminance);
}

void
nouveau_backlight_disable_luminance(const struct drm_connector_state *conn_state)
{
	struct nouveau_connector *nv_connector = nouveau_connector(conn_state->connector);
	struct nouveau_backlight *bl = nv_connector->backlight;

	if (!bl)
		return;

	if (bl->uses_dpcd)
		drm_edp_backlight_disable(&nv_connector->aux, &bl->edp_info);
	else
		nvif_outp_bl_set(&bl->encoder->outp, 0);
}
