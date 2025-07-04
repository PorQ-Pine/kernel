// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Noralf Trønnes
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/lz4.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>
#include <drm/gud.h>

#include "gud_internal.h"

/*
 * Concurrency:
 * Calls into this module from f_gud_drm are serialized and run in process
 * context except gud_gadget_ctrl_get() which is run in interrupt context.
 *
 * Termination:
 * A DRM client can not release itself, only the DRM driver which resources the
 * client uses can do that.
 * This means that there are 2 paths to stop the gadget function:
 * - Unregistering the DRM driver (module unload)
 * - Disabling the USB gadget (configfs unbind)
 *
 * A use counter protects the gadget should the client go away. A kref is used
 * to control the release of the gud_gadget structure shared by the 2 actors.
 *
 * Backlight:
 * If there's a backlight device it's attached to the first connector.
 */

struct gud_gadget_connector {
	struct drm_connector *connector;

	const struct gud_property_req *properties;
	unsigned int num_properties;
	const char *tv_mode_enum_names;
	unsigned int num_tv_mode_enum_names;
	struct backlight_device *backlight;

	spinlock_t lock; /* Protects the following members: */
	enum drm_connector_status status;
	unsigned int width_mm;
	unsigned int height_mm;
	struct gud_display_mode_req *modes;
	unsigned int num_modes;
	void *edid;
	size_t edid_len;
	bool changed;
};

struct gud_gadget {
	struct kref refcount;
	refcount_t usecnt;
	struct drm_client_dev client;
	struct backlight_device *backlight;
	u8 compression;

	const u8 *formats;
	unsigned int format_count;

	const struct gud_property_req *properties;
	unsigned int num_properties;

	struct gud_gadget_connector *connectors;
	unsigned int connector_count;

	struct drm_rect set_buffer_rect;
	u32 set_buffer_length;
	u8 set_buffer_compression;
	u32 set_buffer_compressed_length;

	struct drm_client_buffer *buffer;
	struct drm_client_buffer *buffer_check;
	u8 brightness;
	bool check_ok;

	size_t max_buffer_size;
	void *work_buf;

	struct work_struct flush_worker;
	struct drm_rect flush_rect;
};

static int gud_gadget_probe_connector(struct gud_gadget_connector *gconn)
{
	struct drm_connector *connector = gconn->connector;
	struct gud_display_mode_req *modes = NULL;
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode;
	void *edid_data, *edid = NULL;
	unsigned int num_modes = 0;
	size_t edid_len = 0;
	unsigned long flags;
	unsigned int i = 0;
	int ret = 0;

	mutex_lock(&drm->mode_config.mutex);

	connector->funcs->fill_modes(connector,
				     drm->mode_config.max_width,
				     drm->mode_config.max_height);

	list_for_each_entry(mode, &connector->modes, head)
		num_modes++;

	if (!num_modes)
		goto update;

	modes = kmalloc_array(num_modes, sizeof(*modes), GFP_KERNEL);
	if (!modes) {
		ret = -ENOMEM;
		num_modes = 0;
		goto update;
	}

	list_for_each_entry(mode, &connector->modes, head)
		gud_from_display_mode(&modes[i++], mode);

	if (!connector->edid_blob_ptr)
		goto update;

	edid_data = connector->edid_blob_ptr->data;
	edid_len = connector->edid_blob_ptr->length;
	if (!edid_data || !edid_len) {
		edid_len = 0;
		goto update;
	}

	edid = kmemdup(edid_data, edid_len, GFP_KERNEL);
	if (!edid) {
		ret = -ENOMEM;
		edid_len = 0;
	}

update:
	spin_lock_irqsave(&gconn->lock, flags);
	if (gconn->status != connector->status || gconn->num_modes != num_modes ||
	    gconn->edid_len != edid_len ||
	    (gconn->modes && modes && memcmp(gconn->modes, modes, num_modes * sizeof(*modes))) ||
	    (gconn->edid && edid && memcmp(gconn->edid, edid, edid_len)))
		gconn->changed = true;
	swap(gconn->modes, modes);
	gconn->num_modes = num_modes;
	swap(gconn->edid, edid);
	gconn->edid_len = edid_len;
	gconn->width_mm = connector->display_info.width_mm;
	gconn->height_mm = connector->display_info.height_mm;
	gconn->status = connector->status;
	spin_unlock_irqrestore(&gconn->lock, flags);

	mutex_unlock(&drm->mode_config.mutex);

	kfree(edid);
	kfree(modes);

	return ret;
}

static void gud_gadget_probe_connectors(struct gud_gadget *gdg)
{
	unsigned int i;

	for (i = 0; i < gdg->connector_count; i++)
		gud_gadget_probe_connector(&gdg->connectors[i]);
}

static bool gud_gadget_check_buffer(struct gud_gadget *gdg, struct drm_client_buffer *buffer,
				    struct drm_display_mode *mode, u32 format)
{
	struct drm_framebuffer *fb;

	if (!buffer)
		return false;

	fb = buffer->fb;

	return fb->format->format == format &&
	       fb->width == mode->hdisplay && fb->height == mode->vdisplay;
}

static bool gud_gadget_set_connector_property(struct drm_client_dev *client,
					      struct drm_connector *connector,
					      u16 prop, u64 val, int *ret)
{
	struct drm_mode_config *config = &connector->dev->mode_config;
	struct drm_property *property;

	switch (prop) {
	case GUD_PROPERTY_TV_LEFT_MARGIN:
		property = config->tv_left_margin_property;
		break;
	case GUD_PROPERTY_TV_RIGHT_MARGIN:
		property = config->tv_right_margin_property;
		break;
	case GUD_PROPERTY_TV_TOP_MARGIN:
		property = config->tv_top_margin_property;
		break;
	case GUD_PROPERTY_TV_BOTTOM_MARGIN:
		property = config->tv_bottom_margin_property;
		break;
	case GUD_PROPERTY_TV_MODE:
		property = config->tv_mode_property;
		break;
	case GUD_PROPERTY_TV_BRIGHTNESS:
		property = config->tv_brightness_property;
		break;
	case GUD_PROPERTY_TV_CONTRAST:
		property = config->tv_contrast_property;
		break;
	case GUD_PROPERTY_TV_FLICKER_REDUCTION:
		property = config->tv_flicker_reduction_property;
		break;
	case GUD_PROPERTY_TV_OVERSCAN:
		property = config->tv_overscan_property;
		break;
	case GUD_PROPERTY_TV_SATURATION:
		property = config->tv_saturation_property;
		break;
	case GUD_PROPERTY_TV_HUE:
		property = config->tv_hue_property;
		break;
	default:
		return false;
	}

	*ret = drm_client_modeset_set_property(client, &connector->base, property, val);

	return true;
}

static int gud_gadget_req_set_state_check(struct gud_gadget *gdg, unsigned int index,
					  const struct gud_state_req *req, size_t size)
{
	struct drm_client_dev *client = &gdg->client;
	struct drm_client_buffer *buffer;
	struct drm_connector *connector;
	unsigned int i, num_properties;
	struct drm_display_mode mode;
	u32 format;
	int ret;

	flush_work(&gdg->flush_worker);

	if (index || size < sizeof(*req))
		return -EPROTO;

	if ((size - sizeof(*req)) % sizeof(*req->properties))
		return -EPROTO;

	num_properties = (size - sizeof(*req)) / sizeof(*req->properties);

	memset(&mode, 0, sizeof(mode));
	gud_to_display_mode(&mode, &req->mode);

	gdg->check_ok = false;

	if (!mode.hdisplay || !mode.vdisplay || req->format <= GUD_PIXEL_FORMAT_R1)
		return -EINVAL;

	format = gud_to_fourcc(req->format);
	if (!format)
		return -EINVAL;

	if (req->connector >= gdg->connector_count)
		return -EINVAL;

	connector = gdg->connectors[req->connector].connector;

	if (gdg->buffer_check) {
		drm_client_framebuffer_delete(gdg->buffer_check);
		gdg->buffer_check = NULL;
	}

	if (!gud_gadget_check_buffer(gdg, gdg->buffer, &mode, format)) {
		buffer = drm_client_framebuffer_create(client, mode.hdisplay, mode.vdisplay,
						       format);
		if (IS_ERR(buffer))
			return PTR_ERR(buffer);

		gdg->buffer_check = buffer;
	} else {
		buffer = gdg->buffer;
	}

	ret = drm_client_modeset_set(client, connector, &mode, buffer->fb);
	if (ret)
		return ret;

	for (i = 0; i < num_properties; i++) {
		u16 prop = le16_to_cpu(req->properties[i].prop);
		u64 val = le64_to_cpu(req->properties[i].val);

		if (gud_gadget_set_connector_property(client, connector, prop, val, &ret)) {
			if (ret)
				return ret;
			continue;
		}

		switch (prop) {
		case GUD_PROPERTY_BACKLIGHT_BRIGHTNESS:
			if (val > 100)
				return -EINVAL;
			gdg->brightness = val;
			break;
		case GUD_PROPERTY_ROTATION:
			/* DRM UAPI matches the protocol so use value directly */
			ret = drm_client_modeset_set_rotation(client, val);
			break;
		default:
			pr_err("%s: Unknown property: %u\n", __func__, prop);
			continue;
		}

		if (ret)
			return ret;
	}

	ret = drm_client_modeset_check(&gdg->client);
	if (ret)
		return ret;

	gdg->check_ok = true;

	return 0;
}

static int gud_gadget_req_set_state_commit(struct gud_gadget *gdg, unsigned int index, size_t size)
{
	int ret;

	if (index || size)
		return -EPROTO;

	if (!gdg->check_ok)
		return -EINVAL;

	if (gdg->backlight) {
		int val, max_brightness = gdg->backlight->props.max_brightness;

		val = DIV64_U64_ROUND_UP(gdg->brightness * max_brightness, 100);
		ret = backlight_device_set_brightness(gdg->backlight, val);
		if (ret)
			return ret;
	}

	ret = drm_client_modeset_commit(&gdg->client);
	if (ret)
		return ret;

	if (gdg->buffer_check) {
		drm_client_framebuffer_delete(gdg->buffer);
		gdg->buffer = gdg->buffer_check;
		gdg->buffer_check = NULL;
	}

	return 0;
}

/*
 * Use a worker so the next frame can be received while the current frame is being flushed out.
 * SPI panels often use 50ms to flush a full frame.
 */
static void gud_gadget_flush_worker(struct work_struct *work)
{
	struct gud_gadget *gdg = container_of(work, struct gud_gadget, flush_worker);
	struct drm_client_buffer *buffer = gdg->buffer ? gdg->buffer : gdg->buffer_check;
	int ret;

	ret = drm_client_framebuffer_flush(buffer, &gdg->flush_rect);
	if (ret)
		pr_debug("%s: drm_client_framebuffer_flush: error=%d\n", __func__, ret);
}

static size_t gud_gadget_write_buffer_memcpy(struct drm_client_buffer *buffer,
					     const void *src, size_t len,
					     struct drm_rect *rect)
{
	unsigned int cpp = buffer->fb->format->cpp[0];
	size_t src_pitch = drm_rect_width(rect) * cpp;
	size_t dst_pitch = buffer->fb->pitches[0];
	struct iosys_map dst;
	unsigned int y;
	int ret;

	ret = drm_client_buffer_vmap(buffer, &dst);
	if (ret)
		return len;

	iosys_map_incr(&dst, rect->y1 * dst_pitch + rect->x1 * cpp);

	for (y = 0; y < drm_rect_height(rect) && len; y++) {
		src_pitch = min(src_pitch, len);
		iosys_map_memcpy_to(&dst, 0, src, src_pitch);
		iosys_map_incr(&dst, dst_pitch);
		src += src_pitch;
		len -= src_pitch;
	}

	drm_client_buffer_vunmap(buffer);

	return len;
}

static bool gud_gadget_check_rect(struct drm_client_buffer *buffer, struct drm_rect *rect)
{
	return buffer->fb && rect->x1 < rect->x2 && rect->y1 < rect->y2 &&
	       rect->x2 <= buffer->fb->width && rect->y2 <= buffer->fb->height;
}

int gud_gadget_write_buffer(struct gud_gadget *gdg, const void *buf, size_t len)
{
	struct drm_client_buffer *buffer = gdg->buffer ? gdg->buffer : gdg->buffer_check;
	struct drm_rect *rect = &gdg->set_buffer_rect;
	u8 compression = gdg->set_buffer_compression;
	struct drm_framebuffer *fb;
	size_t remain;
	int ret;

	pr_debug("%s: len=%zu compression=0x%x\n", __func__, len, compression);

	if (!refcount_inc_not_zero(&gdg->usecnt))
		return -ENODEV;

	if (WARN_ON_ONCE(!buffer)) {
		ret = -ENOMEM;
		goto out;
	}

	if (!gud_gadget_check_rect(buffer, rect)) {
		pr_err("%s: Rectangle doesn't fit: " DRM_RECT_FMT "\n",
		       __func__, DRM_RECT_ARG(rect));
		ret = -EINVAL;
		goto out;
	}

	fb = buffer->fb;

	if (fb->funcs->dirty)
		flush_work(&gdg->flush_worker);

	if (compression & GUD_COMPRESSION_LZ4) {
		if (len != gdg->set_buffer_compressed_length) {
			pr_err("%s: Buffer compressed len differs: %zu != %u\n",
			       __func__, len, gdg->set_buffer_compressed_length);
			ret = -EINVAL;
			goto out;
		}

		ret = LZ4_decompress_safe(buf, gdg->work_buf, len, gdg->max_buffer_size);
		if (ret < 0) {
			pr_err("%s: Failed to decompress buffer\n", __func__);
			ret = -EIO;
			goto out;
		}

		buf = gdg->work_buf;
		len = ret;
	}

	if (len != gdg->set_buffer_length) {
		pr_err("%s: Buffer len differs: %zu != %u\n",
		       __func__, len, gdg->set_buffer_length);
		ret = -EINVAL;
		goto out;
	}

	remain = gud_gadget_write_buffer_memcpy(buffer, buf, len, rect);
	if (remain) {
		pr_err("%s: Failed to write buffer: remain=%zu\n", __func__, remain);
		ret = -EIO;
		goto out;
	}

	if (fb->funcs->dirty) {
		gdg->flush_rect = *rect;
		queue_work(system_long_wq, &gdg->flush_worker);
	}

	ret = 0;
out:
	refcount_dec(&gdg->usecnt);

	return ret;
}
EXPORT_SYMBOL(gud_gadget_write_buffer);

int gud_gadget_req_set_buffer(struct gud_gadget *gdg, const struct gud_set_buffer_req *req)
{
	u32 compressed_length = le32_to_cpu(req->compressed_length);
	u32 length = le32_to_cpu(req->length);
	struct drm_client_buffer *buffer;
	struct drm_rect rect;
	int ret = 0;
	u64 pitch;

	if (!refcount_inc_not_zero(&gdg->usecnt))
		return -ENODEV;

	buffer = gdg->buffer ? gdg->buffer : gdg->buffer_check;
	if (!buffer) {
		ret = -ENOENT;
		goto out;
	}

	drm_rect_init(&rect, le32_to_cpu(req->x), le32_to_cpu(req->y),
		      le32_to_cpu(req->width), le32_to_cpu(req->height));

	pr_debug("%s: " DRM_RECT_FMT "\n", __func__, DRM_RECT_ARG(&rect));

	if (!gud_gadget_check_rect(buffer, &rect)) {
		ret = -EINVAL;
		goto out;
	}

	if (req->compression & ~GUD_COMPRESSION_LZ4) {
		ret = -EINVAL;
		goto out;
	}

	gdg->set_buffer_rect = rect;
	gdg->set_buffer_length = length;

	if (req->compression) {
		if (!compressed_length) {
			ret = -EINVAL;
			goto out;
		}
		gdg->set_buffer_compression = req->compression;
		gdg->set_buffer_compressed_length = compressed_length;
		length = compressed_length;
	} else {
		gdg->set_buffer_compression = 0;
		gdg->set_buffer_compressed_length = 0;
	}

	pitch = drm_format_info_min_pitch(buffer->fb->format, 0, drm_rect_width(&rect));
	if (length > (drm_rect_height(&rect) * pitch)) {
		pr_err("%s: Buffer is too big for rectangle: " DRM_RECT_FMT " len=%u\n",
		       __func__, DRM_RECT_ARG(&rect), length);
		ret = -EINVAL;
		goto out;
	}
out:
	refcount_dec(&gdg->usecnt);

	return ret ? ret : length;
}
EXPORT_SYMBOL(gud_gadget_req_set_buffer);

static void gud_gadget_delete_buffers(struct gud_gadget *gdg)
{
	drm_client_framebuffer_delete(gdg->buffer_check);
	drm_client_framebuffer_delete(gdg->buffer);
	gdg->buffer_check = NULL;
	gdg->buffer = NULL;
}

int gud_gadget_disable_pipe(struct gud_gadget *gdg)
{
	int ret;

	cancel_work_sync(&gdg->flush_worker);
	ret = drm_client_modeset_disable(&gdg->client);
	gud_gadget_delete_buffers(gdg);

	return ret;
}
EXPORT_SYMBOL(gud_gadget_disable_pipe);

static int gud_gadget_req_get_descriptor(struct gud_gadget *gdg, unsigned int index,
					 void *data, size_t size)
{
	struct drm_device *drm = gdg->client.dev;
	struct gud_display_descriptor_req desc;

	if (index || !size)
		return -EPROTO;

	desc.magic = cpu_to_le32(GUD_DISPLAY_MAGIC);
	desc.version = 1;
	desc.max_buffer_size = cpu_to_le32(gdg->max_buffer_size);
	desc.flags = cpu_to_le32(GUD_DISPLAY_FLAG_STATUS_ON_SET);
	desc.compression = GUD_COMPRESSION_LZ4 & gdg->compression;

	desc.min_width = cpu_to_le32(drm->mode_config.min_width);
	desc.max_width = cpu_to_le32(drm->mode_config.max_width);
	desc.min_height = cpu_to_le32(drm->mode_config.min_height);
	desc.max_height = cpu_to_le32(drm->mode_config.max_height);

	size = min(size, sizeof(desc));
	memcpy(data, &desc, size);

	return size;
}

static int gud_gadget_req_get_formats(struct gud_gadget *gdg, unsigned int index,
				      void *data, size_t size)
{
	if (index || !size)
		return -EPROTO;

	size = min_t(size_t, size, gdg->format_count);
	memcpy(data, gdg->formats, size);

	return size;
}

static int gud_gadget_req_get_properties(struct gud_gadget *gdg, unsigned int index,
					 void *data, size_t size)
{
	size = rounddown(size, sizeof(*gdg->properties));
	if (index || !size)
		return -EPROTO;

	size = min(size, gdg->num_properties * sizeof(*gdg->properties));
	memcpy(data, gdg->properties, size);

	return size;
}

static void gud_gadget_req_get_connector(struct gud_gadget *gdg, unsigned int index,
					 struct gud_connector_descriptor_req *desc)
{
	struct gud_gadget_connector *gconn;
	u32 flags;

	memset(desc, 0, sizeof(*desc));

	gconn = &gdg->connectors[index];

	switch (gconn->connector->connector_type) {
	case DRM_MODE_CONNECTOR_VGA:
		desc->connector_type = GUD_CONNECTOR_TYPE_VGA;
		break;
	case DRM_MODE_CONNECTOR_DVII:
		fallthrough;
	case DRM_MODE_CONNECTOR_DVID:
		fallthrough;
	case DRM_MODE_CONNECTOR_DVIA:
		desc->connector_type = GUD_CONNECTOR_TYPE_DVI;
		break;
	case DRM_MODE_CONNECTOR_Composite:
		desc->connector_type = GUD_CONNECTOR_TYPE_COMPOSITE;
		break;
	case DRM_MODE_CONNECTOR_SVIDEO:
		desc->connector_type = GUD_CONNECTOR_TYPE_SVIDEO;
		break;
	case DRM_MODE_CONNECTOR_Component:
		desc->connector_type = GUD_CONNECTOR_TYPE_COMPONENT;
		break;
	case DRM_MODE_CONNECTOR_DisplayPort:
		desc->connector_type = GUD_CONNECTOR_TYPE_DISPLAYPORT;
		break;
	case DRM_MODE_CONNECTOR_HDMIA:
		fallthrough;
	case DRM_MODE_CONNECTOR_HDMIB:
		desc->connector_type = GUD_CONNECTOR_TYPE_HDMI;
		break;
	default:
		desc->connector_type = GUD_CONNECTOR_TYPE_PANEL;
		break;
	};

	flags = GUD_CONNECTOR_FLAGS_POLL_STATUS;
	if (gconn->connector->interlace_allowed)
		flags |= GUD_CONNECTOR_FLAGS_INTERLACE;
	if (gconn->connector->doublescan_allowed)
		flags |= GUD_CONNECTOR_FLAGS_DOUBLESCAN;
	desc->flags = cpu_to_le32(flags);
}

static int gud_gadget_req_get_connectors(struct gud_gadget *gdg, unsigned int index,
					 struct gud_connector_descriptor_req *descs, size_t size)
{
	unsigned int i, num_connectors;

	size = rounddown(size, sizeof(*descs));
	if (index || !size)
		return -EPROTO;

	num_connectors = min_t(size_t, size / sizeof(*descs), gdg->connector_count);

	for (i = 0; i < num_connectors; i++)
		gud_gadget_req_get_connector(gdg, i, &descs[i]);

	return num_connectors * sizeof(*descs);
}

static struct gud_gadget_connector *
gud_gadget_get_gconn(struct gud_gadget *gdg, unsigned int index)
{
	if (index >= gdg->connector_count)
		return NULL;

	return &gdg->connectors[index];
}

static int gud_gadget_req_get_connector_properties(struct gud_gadget *gdg, unsigned int index,
						   void *data, size_t size)
{
	struct gud_gadget_connector *gconn;

	size = rounddown(size, sizeof(*gconn->properties));
	if (!size)
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	size = min(size, gconn->num_properties * sizeof(*gconn->properties));
	memcpy(data, gconn->properties, size);

	return size;
}

static int gud_gadget_req_get_connector_tv_mode_values(struct gud_gadget *gdg, unsigned int index,
						       void *data, size_t size)
{
	struct gud_gadget_connector *gconn;

	size = rounddown(size, GUD_CONNECTOR_TV_MODE_NAME_LEN);
	if (!size)
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	size = min_t(size_t, size, gconn->num_tv_mode_enum_names * GUD_CONNECTOR_TV_MODE_NAME_LEN);
	memcpy(data, gconn->tv_mode_enum_names, size);

	return size;
}

static int gud_gadget_req_get_connector_status(struct gud_gadget *gdg, unsigned int index,
					       u8 *status, size_t size)
{
	struct gud_gadget_connector *gconn;
	unsigned long flags;

	if (size != sizeof(*status))
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	spin_lock_irqsave(&gconn->lock, flags);

	switch (gconn->status) {
	case connector_status_disconnected:
		*status = GUD_CONNECTOR_STATUS_DISCONNECTED;
		break;
	case connector_status_connected:
		*status = GUD_CONNECTOR_STATUS_CONNECTED;
		break;
	default:
		*status = GUD_CONNECTOR_STATUS_UNKNOWN;
		break;

	};

	if (gconn->changed) {
		*status |= GUD_CONNECTOR_STATUS_CHANGED;
		gconn->changed = false;
	}

	spin_unlock_irqrestore(&gconn->lock, flags);

	return size;
}

static int gud_gadget_req_get_connector_modes(struct gud_gadget *gdg, unsigned int index,
					      void *data, size_t size)
{
	struct gud_gadget_connector *gconn;
	unsigned long flags;

	size = rounddown(size, sizeof(*gconn->modes));
	if (!size)
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	spin_lock_irqsave(&gconn->lock, flags);
	size = min(size, gconn->num_modes * sizeof(*gconn->modes));
	memcpy(data, gconn->modes, size);
	spin_unlock_irqrestore(&gconn->lock, flags);

	return size;
}

static int gud_gadget_req_get_connector_edid(struct gud_gadget *gdg, unsigned int index,
					     void *data, size_t size)
{
	struct gud_gadget_connector *gconn;
	unsigned long flags;

	size = rounddown(size, EDID_LENGTH);
	if (!size)
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	spin_lock_irqsave(&gconn->lock, flags);
	size = min(size, gconn->edid_len);
	memcpy(data, gconn->edid, size);
	spin_unlock_irqrestore(&gconn->lock, flags);

	return size;
}

/* This runs in interrupt context */
int gud_gadget_req_get(struct gud_gadget *gdg, u8 request, u16 index, void *data, size_t size)
{
	int ret;

	pr_debug("%s: request=0x%x index=%u size=%zu\n", __func__, request, index, size);

	if (!refcount_inc_not_zero(&gdg->usecnt))
		return -ENODEV;

	switch (request) {
	case GUD_REQ_GET_DESCRIPTOR:
		ret = gud_gadget_req_get_descriptor(gdg, index, data, size);
		break;
	case GUD_REQ_GET_FORMATS:
		ret = gud_gadget_req_get_formats(gdg, index, data, size);
		break;
	case GUD_REQ_GET_PROPERTIES:
		ret = gud_gadget_req_get_properties(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTORS:
		ret = gud_gadget_req_get_connectors(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTOR_PROPERTIES:
		ret = gud_gadget_req_get_connector_properties(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTOR_TV_MODE_VALUES:
		ret = gud_gadget_req_get_connector_tv_mode_values(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTOR_STATUS:
		ret = gud_gadget_req_get_connector_status(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTOR_MODES:
		ret = gud_gadget_req_get_connector_modes(gdg, index, data, size);
		break;
	case GUD_REQ_GET_CONNECTOR_EDID:
		ret = gud_gadget_req_get_connector_edid(gdg, index, data, size);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	refcount_dec(&gdg->usecnt);

	return ret;
}
EXPORT_SYMBOL(gud_gadget_req_get);

static int gud_gadget_req_set_connector_force_detect(struct gud_gadget *gdg, u16 index, size_t size)
{
	struct gud_gadget_connector *gconn;

	if (size)
		return -EPROTO;

	gconn = gud_gadget_get_gconn(gdg, index);
	if (!gconn)
		return -EINVAL;

	return gud_gadget_probe_connector(gconn);
}

static int gud_gadget_req_set_controller_enable(struct gud_gadget *gdg, u16 index,
						const u8 *enable, size_t size)
{
	if (index || size != sizeof(*enable))
		return -EPROTO;

	return *enable ? 0 : gud_gadget_disable_pipe(gdg);
}

static int gud_gadget_req_set_display_enable(struct gud_gadget *gdg, u16 index,
					     const u8 *enable, size_t size)
{
	if (index || size != sizeof(*enable))
		return -EPROTO;

	return drm_client_modeset_dpms(&gdg->client,
				       *enable ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
}

int gud_gadget_req_set(struct gud_gadget *gdg, u8 request, u16 index, const void *data, size_t size)
{
	int ret;

	pr_debug("%s: request=0x%x index=%u size=%zu\n", __func__, request, index, size);

	if (!refcount_inc_not_zero(&gdg->usecnt))
		return -ENODEV;

	switch (request) {
	case GUD_REQ_SET_CONNECTOR_FORCE_DETECT:
		ret = gud_gadget_req_set_connector_force_detect(gdg, index, size);
		break;
	case GUD_REQ_SET_STATE_CHECK:
		ret = gud_gadget_req_set_state_check(gdg, index, data, size);
		break;
	case GUD_REQ_SET_STATE_COMMIT:
		ret = gud_gadget_req_set_state_commit(gdg, index, size);
		break;
	case GUD_REQ_SET_CONTROLLER_ENABLE:
		ret = gud_gadget_req_set_controller_enable(gdg, index, data, size);
		break;
	case GUD_REQ_SET_DISPLAY_ENABLE:
		ret = gud_gadget_req_set_display_enable(gdg, index, data, size);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	refcount_dec(&gdg->usecnt);

	return ret;
}
EXPORT_SYMBOL(gud_gadget_req_set);

static int gud_gadget_get_formats(struct gud_gadget *gdg, u8 *max_cpp, u8 *format_filter)
{
	struct drm_device *drm = gdg->client.dev;
	struct drm_plane *plane;
	unsigned int i;
	u8 *formats;
	int ret;

	*max_cpp = 0;

	drm_for_each_plane(plane, drm) {
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			break;
	}

	formats = kmalloc(plane->format_count, GFP_KERNEL);
	if (!formats)
		return -ENOMEM;

	for (i = 0; i < plane->format_count; i++) {
		const struct drm_format_info *info;
		u8 format;

		info = drm_format_info(plane->format_types[i]);
		if (info->num_planes != 1)
			continue;

		format = gud_from_fourcc(info->format);
		if (!format)
			continue;

		if (format_filter[0]) {
			bool found = false;
			unsigned int j;

			for (j = 0; j < GUD_FORMATS_MAX_NUM; j++) {
				if (!format_filter[j])
					break;
				if (format_filter[j] == format) {
					found = true;
					break;
				}
			}
			if (!found)
				continue;
		}

		if (*max_cpp < info->cpp[0])
			*max_cpp = info->cpp[0];

		formats[gdg->format_count++] = format;
	}

	if (!gdg->format_count) {
		ret = -ENOENT;
		goto err_free;
	}

	gdg->formats = formats;

	return 0;

err_free:
	kfree(formats);

	return ret;
}

static int gud_gadget_get_rotation_property(struct drm_device *drm, u16 *prop, u64 *val)
{
	struct drm_property_enum *prop_enum;
	struct drm_plane *plane;
	unsigned int num_props = 0;
	u16 bitmask = 0;

	drm_for_each_plane(plane, drm) {
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			break;
	}

	if (!plane->rotation_property)
		return 0;

	list_for_each_entry(prop_enum, &plane->rotation_property->enum_list, head) {
		num_props++;
		bitmask |= BIT(prop_enum->value);
	}

	*prop = GUD_PROPERTY_ROTATION;
	/* DRM UAPI matches the protocol so use value directly */
	*val = bitmask;

	return 1;
}

static int gud_gadget_get_properties(struct gud_gadget *gdg)
{
	struct gud_property_req *properties;
	u16 prop;
	u64 val;
	int ret;

	ret = gud_gadget_get_rotation_property(gdg->client.dev, &prop, &val);
	if (ret <= 0)
		return ret;

	properties = kcalloc(1, sizeof(*properties), GFP_KERNEL);
	if (!properties)
		return -ENOMEM;

	gdg->properties = properties;
	gdg->num_properties++;

	properties[0].prop = cpu_to_le16(prop);
	properties[0].val = cpu_to_le64(val);

	return 0;
}

static int gud_gadget_get_connector_properties(struct gud_gadget *gdg,
					       struct gud_gadget_connector *gconn)
{
	struct drm_device *drm = gdg->client.dev;
	struct drm_mode_config *config = &drm->mode_config;
	struct drm_connector *connector = gconn->connector;
	struct drm_object_properties *conn_props = connector->base.properties;
	struct gud_property_req *properties;
	unsigned int i, ret = 0;
	u16 prop;
	u64 val;

	mutex_lock(&drm->mode_config.mutex);

	if (!conn_props->count)
		goto unlock;

	/* Add room for possible backlight */
	properties = kcalloc(conn_props->count + 1, sizeof(*properties), GFP_KERNEL);
	if (!properties) {
		ret = -ENOMEM;
		goto unlock;
	}

	gconn->properties = properties;

	for (i = 0; i < conn_props->count; i++) {
		struct drm_property *property = conn_props->properties[i];

		if (property == config->tv_left_margin_property) {
			prop = GUD_PROPERTY_TV_LEFT_MARGIN;
			val = connector->state->tv.margins.left;
		} else if (property == config->tv_right_margin_property) {
			prop = GUD_PROPERTY_TV_RIGHT_MARGIN;
			val = connector->state->tv.margins.right;
		} else if (property == config->tv_top_margin_property) {
			prop = GUD_PROPERTY_TV_TOP_MARGIN;
			val = connector->state->tv.margins.top;
		} else if (property == config->tv_bottom_margin_property) {
			prop = GUD_PROPERTY_TV_BOTTOM_MARGIN;
			val = connector->state->tv.margins.bottom;
		} else if (property == config->tv_mode_property) {
			struct drm_property_enum *prop_enum;
			char *buf;

			list_for_each_entry(prop_enum, &property->enum_list, head)
				gconn->num_tv_mode_enum_names++;

			if (WARN_ON(!gconn->num_tv_mode_enum_names)) {
				ret = -EINVAL;
				goto unlock;
			}

			buf = kcalloc(gconn->num_tv_mode_enum_names,
				      GUD_CONNECTOR_TV_MODE_NAME_LEN, GFP_KERNEL);
			if (!buf) {
				ret = -ENOMEM;
				goto unlock;
			}

			gconn->tv_mode_enum_names = buf;

			list_for_each_entry(prop_enum, &property->enum_list, head) {
				strscpy(buf, prop_enum->name, GUD_CONNECTOR_TV_MODE_NAME_LEN);
				buf += GUD_CONNECTOR_TV_MODE_NAME_LEN;
			}

			prop = GUD_PROPERTY_TV_MODE;
			val = connector->state->tv.mode;
		} else if (property == config->tv_brightness_property) {
			prop = GUD_PROPERTY_TV_BRIGHTNESS;
			val = connector->state->tv.brightness;
		} else if (property == config->tv_contrast_property) {
			prop = GUD_PROPERTY_TV_CONTRAST;
			val = connector->state->tv.contrast;
		} else if (property == config->tv_flicker_reduction_property) {
			prop = GUD_PROPERTY_TV_FLICKER_REDUCTION;
			val = connector->state->tv.flicker_reduction;
		} else if (property == config->tv_overscan_property) {
			prop = GUD_PROPERTY_TV_OVERSCAN;
			val = connector->state->tv.overscan;
		} else if (property == config->tv_saturation_property) {
			prop = GUD_PROPERTY_TV_SATURATION;
			val = connector->state->tv.saturation;
		} else if (property == config->tv_hue_property) {
			prop = GUD_PROPERTY_TV_HUE;
			val = connector->state->tv.hue;
		} else {
			continue;
		}

		properties[gconn->num_properties].prop = cpu_to_le16(prop);
		properties[gconn->num_properties++].val = cpu_to_le64(val);
	}

	if (!connector->index && gdg->backlight) {
		struct backlight_properties *props = &gdg->backlight->props;

		prop = GUD_PROPERTY_BACKLIGHT_BRIGHTNESS;
		val = DIV64_U64_ROUND_UP(props->brightness * 100, props->max_brightness);
		properties[gconn->num_properties].prop = cpu_to_le16(prop);
		properties[gconn->num_properties++].val = cpu_to_le64(val);
		gconn->backlight = gdg->backlight;
	}
unlock:
	mutex_unlock(&drm->mode_config.mutex);

	return ret;
}

static int gud_gadget_get_connectors(struct gud_gadget *gdg, u32 connectors_mask)
{
	struct gud_gadget_connector *connectors = NULL;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *drm = gdg->client.dev;
	unsigned int connector_count = 0;
	struct drm_connector *connector;
	int ret = 0;

	drm_connector_list_iter_begin(drm, &conn_iter);
	drm_client_for_each_connector_iter(connector, &conn_iter) {
		struct gud_gadget_connector *tmp, *gconn;

		if (!((BIT(connector->index)) & connectors_mask))
			continue;

		tmp = krealloc(connectors, (connector_count + 1) * sizeof(*connectors),
			       GFP_KERNEL | __GFP_ZERO);
		if (!tmp) {
			ret = -ENOMEM;
			break;
		}

		connectors = tmp;
		drm_connector_get(connector);
		gconn = &connectors[connector_count++];
		gconn->connector = connector;
		spin_lock_init(&gconn->lock);

		ret = gud_gadget_get_connector_properties(gdg, gconn);
		if (ret)
			break;
	}
	drm_connector_list_iter_end(&conn_iter);

	if (!connector_count)
		ret = -ENOENT;

	gdg->connectors = connectors;
	gdg->connector_count = connector_count;

	return ret;
}

static void gud_gadget_release(struct kref *kref)
{
	struct gud_gadget *gdg = container_of(kref, struct gud_gadget, refcount);

	kfree(gdg);
}

static void gud_gadget_put(struct gud_gadget *gdg)
{
	kref_put(&gdg->refcount, gud_gadget_release);
}

static void gud_gadget_client_unregister(struct drm_client_dev *client)
{
	struct gud_gadget *gdg = container_of(client, struct gud_gadget, client);
	int timeout = 10000 / 50;
	unsigned int i;

	/*
	 * If usecnt doesn't drop to zero, try waiting for the gadget, but we
	 * can't block the DRM driver forever. The worst case wait the gadget side
	 * can hit are tens of seconds through the call to drm_client_modeset_commit().
	 */
	if (refcount_dec_and_test(&gdg->usecnt)) {
		for (; timeout && refcount_read(&gdg->usecnt); timeout--)
			msleep(50);
	}

	if (!timeout) {
		pr_err("%s: Timeout waiting for gadget side, will leak memory\n", __func__);
		return;
	}

	vfree(gdg->work_buf);
	kfree(gdg->formats);
	kfree(gdg->properties);

	for (i = 0; i < gdg->connector_count; i++) {
		struct gud_gadget_connector *gconn = &gdg->connectors[i];

		drm_connector_put(gconn->connector);
		kfree(gconn->properties);
		kfree(gconn->tv_mode_enum_names);
		kfree(gconn->modes);
		kfree(gconn->edid);
	}
	kfree(gdg->connectors);

	gud_gadget_delete_buffers(gdg);
	drm_client_release(client);
	gud_gadget_put(gdg);
}

static int gud_gadget_client_hotplug(struct drm_client_dev *client)
{
	struct gud_gadget *gdg = container_of(client, struct gud_gadget, client);

	gud_gadget_probe_connectors(gdg);

	return 0;
}

static const struct drm_client_funcs gdg_client_funcs = {
	.owner		= THIS_MODULE,
	.unregister	= gud_gadget_client_unregister,
	.hotplug	= gud_gadget_client_hotplug,
};

struct gud_gadget *gud_gadget_init(unsigned int minor_id, const char *backlight,
				   size_t *max_buffer_size, u8 compression, u8 *formats,
				   u32 connectors)
{
	struct gud_gadget *gdg;
	u8 max_cpp;
	int ret;

	gdg = kzalloc(sizeof(*gdg), GFP_KERNEL);
	if (!gdg)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&gdg->flush_worker, gud_gadget_flush_worker);
	gdg->compression = compression;

	ret = drm_client_init_from_id(minor_id, &gdg->client, "gud-drm-gadget", &gdg_client_funcs);
	if (ret) {
		pr_err("Failed to aquire minor=%u\n", minor_id);
		kfree(gdg);
		return ERR_PTR(ret);
	}

	refcount_set(&gdg->usecnt, 1);
	/* The DRM driver (through the client) and f_gud_drm need one ref each */
	kref_init(&gdg->refcount);
	kref_get(&gdg->refcount);

	if (backlight) {
		gdg->backlight = backlight_device_get_by_name(backlight);
		if (!gdg->backlight) {
			pr_err("Failed to find backlight: %s\n", backlight);
			ret = -ENODEV;
			goto error_release;
		}
	}

	ret = gud_gadget_get_formats(gdg, &max_cpp, formats);
	if (ret) {
		pr_err("Failed to get formats\n");
		goto error_release;
	}

	*max_buffer_size = gdg->client.dev->mode_config.max_width *
			   gdg->client.dev->mode_config.max_height * max_cpp;
	/* f_gud_drm will kmalloc a buffer of this size */
	*max_buffer_size = min_t(size_t, *max_buffer_size, KMALLOC_MAX_SIZE);

	gdg->max_buffer_size = *max_buffer_size;
	gdg->work_buf = vmalloc(gdg->max_buffer_size);
	if (!gdg->work_buf) {
		ret = -ENOMEM;
		goto error_release;
	}

	ret = gud_gadget_get_properties(gdg);
	if (ret) {
		pr_err("Failed to get properties\n");
		goto error_release;
	}

	ret = gud_gadget_get_connectors(gdg, connectors);
	if (ret) {
		pr_err("Failed to get connectors\n");
		goto error_release;
	}

	if (!drm_client_register(&gdg->client)) {
		pr_err("DRM device is gone\n");
		ret = -ENODEV;
		goto error_release;
	}

	gud_gadget_probe_connectors(gdg);

	return gdg;

error_release:
	gud_gadget_client_unregister(&gdg->client);
	gud_gadget_fini(gdg);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL(gud_gadget_init);

void gud_gadget_fini(struct gud_gadget *gdg)
{
	if (gdg->backlight)
		put_device(&gdg->backlight->dev);
	gud_gadget_put(gdg);
}
EXPORT_SYMBOL(gud_gadget_fini);

MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
