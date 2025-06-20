/* SPDX-License-Identifier: GPL-2.0 or MIT */

#ifndef _DRM_CLIENT_H_
#define _DRM_CLIENT_H_

#include <linux/iosys-map.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>

struct drm_client_dev;
struct drm_device;
struct drm_file;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_minor;
struct drm_property;
struct module;

/**
 * struct drm_client_funcs - DRM client callbacks
 */
struct drm_client_funcs {
	/**
	 * @owner: The module owner
	 */
	struct module *owner;

	/**
	 * @unregister:
	 *
	 * Called when &drm_device is unregistered. The client should respond by
	 * releasing its resources using drm_client_release().
	 *
	 * This callback is optional.
	 */
	void (*unregister)(struct drm_client_dev *client);

	/**
	 * @restore:
	 *
	 * Called on drm_lastclose(). The first client instance in the list that
	 * returns zero gets the privilege to restore and no more clients are
	 * called. This callback is not called after @unregister has been called.
	 *
	 * Note that the core does not guarantee exclusion against concurrent
	 * drm_open(). Clients need to ensure this themselves, for example by
	 * using drm_master_internal_acquire() and
	 * drm_master_internal_release().
	 *
	 * This callback is optional.
	 */
	int (*restore)(struct drm_client_dev *client);

	/**
	 * @hotplug:
	 *
	 * Called on drm_kms_helper_hotplug_event().
	 * This callback is not called after @unregister has been called.
	 *
	 * This callback is optional.
	 */
	int (*hotplug)(struct drm_client_dev *client);

	/**
	 * @suspend:
	 *
	 * Called when suspending the device.
	 *
	 * This callback is optional.
	 *
	 * FIXME: Some callers hold the console lock when invoking this
	 *        function. This interferes with fbdev emulation, which
	 *        also tries to acquire the lock. Push the console lock
	 *        into the callback and remove 'holds_console_lock'.
	 */
	int (*suspend)(struct drm_client_dev *client, bool holds_console_lock);

	/**
	 * @resume:
	 *
	 * Called when resuming the device from suspend.
	 *
	 * This callback is optional.
	 *
	 * FIXME: Some callers hold the console lock when invoking this
	 *        function. This interferes with fbdev emulation, which
	 *        also tries to acquire the lock. Push the console lock
	 *        into the callback and remove 'holds_console_lock'.
	 */
	int (*resume)(struct drm_client_dev *client, bool holds_console_lock);
};

/**
 * struct drm_client_property - DRM client property
 */
struct drm_client_property {
	/**
	 * @obj: DRM Mode Object to which the property belongs.
	 */
	struct drm_mode_object *obj;

	/**
	 * @prop: DRM Property.
	 */
	struct drm_property *prop;

	/**
	 * @value: Property value.
	 */
	u64 value;
};

/**
 * struct drm_client_dev - DRM client instance
 */
struct drm_client_dev {
	/**
	 * @dev: DRM device
	 */
	struct drm_device *dev;

	/**
	 * @name: Name of the client.
	 */
	const char *name;

	/**
	 * @list:
	 *
	 * List of all clients of a DRM device, linked into
	 * &drm_device.clientlist. Protected by &drm_device.clientlist_mutex.
	 */
	struct list_head list;

	/**
	 * @funcs: DRM client functions (optional)
	 */
	const struct drm_client_funcs *funcs;

	/**
	 * @file: DRM file
	 */
	struct drm_file *file;

	/**
	 * @modeset_mutex: Protects @modesets and @properties.
	 */
	struct mutex modeset_mutex;

	/**
	 * @modesets: CRTC configurations
	 */
	struct drm_mode_set *modesets;

	/**
	 * @suspended:
	 *
	 * The client has been suspended.
	 */
	bool suspended;

	/**
	 * @hotplug_pending:
	 *
	 * A hotplug event has been received while the client was suspended.
	 * Try again on resume.
	 */
	bool hotplug_pending;

	/**
	 * @properties: DRM properties attached to the configuration.
	 */
	struct drm_client_property *properties;

	/**
	 * @num_properties: Number of attached properties.
	 */
	unsigned int num_properties;

	/**
	 * @hotplug_failed:
	 *
	 * Set by client hotplug helpers if the hotplugging failed
	 * before. It is usually not tried again.
	 */
	bool hotplug_failed;
};

int drm_client_init(struct drm_device *dev, struct drm_client_dev *client,
		    const char *name, const struct drm_client_funcs *funcs);
int drm_client_init_from_id(unsigned int minor_id, struct drm_client_dev *client,
		    const char *name, const struct drm_client_funcs *funcs);
void drm_client_release(struct drm_client_dev *client);
bool drm_client_register(struct drm_client_dev *client);

/**
 * struct drm_client_buffer - DRM client buffer
 */
struct drm_client_buffer {
	/**
	 * @client: DRM client
	 */
	struct drm_client_dev *client;

	/**
	 * @pitch: Buffer pitch
	 */
	u32 pitch;

	/**
	 * @gem: GEM object backing this buffer
	 *
	 * FIXME: The dependency on GEM here isn't required, we could
	 * convert the driver handle to a dma-buf instead and use the
	 * backend-agnostic dma-buf vmap support instead. This would
	 * require that the handle2fd prime ioctl is reworked to pull the
	 * fd_install step out of the driver backend hooks, to make that
	 * final step optional for internal users.
	 */
	struct drm_gem_object *gem;

	/**
	 * @map: Virtual address for the buffer
	 */
	struct iosys_map map;

	/**
	 * @fb: DRM framebuffer
	 */
	struct drm_framebuffer *fb;
};

struct drm_client_buffer *
drm_client_framebuffer_create(struct drm_client_dev *client, u32 width, u32 height, u32 format);
void drm_client_framebuffer_delete(struct drm_client_buffer *buffer);
int drm_client_framebuffer_flush(struct drm_client_buffer *buffer, struct drm_rect *rect);
int drm_client_buffer_vmap_local(struct drm_client_buffer *buffer,
				 struct iosys_map *map_copy);
void drm_client_buffer_vunmap_local(struct drm_client_buffer *buffer);
int drm_client_buffer_vmap(struct drm_client_buffer *buffer,
			   struct iosys_map *map);
void drm_client_buffer_vunmap(struct drm_client_buffer *buffer);

int drm_client_modeset_create(struct drm_client_dev *client);
void drm_client_modeset_free(struct drm_client_dev *client);
int drm_client_modeset_probe(struct drm_client_dev *client, unsigned int width, unsigned int height);
int drm_client_modeset_set(struct drm_client_dev *client, struct drm_connector *connector,
			   struct drm_display_mode *mode, struct drm_framebuffer *fb);
int drm_client_modeset_set_property(struct drm_client_dev *client, struct drm_mode_object *obj,
				    struct drm_property *prop, u64 value);
int drm_client_modeset_set_rotation(struct drm_client_dev *client, u64 value);
bool drm_client_rotation(struct drm_mode_set *modeset, unsigned int *rotation);
int drm_client_modeset_check(struct drm_client_dev *client);
int drm_client_modeset_commit_locked(struct drm_client_dev *client);
int drm_client_modeset_commit(struct drm_client_dev *client);
int drm_client_modeset_dpms(struct drm_client_dev *client, int mode);
int drm_client_modeset_disable(struct drm_client_dev *client);

/**
 * drm_client_for_each_modeset() - Iterate over client modesets
 * @modeset: &drm_mode_set loop cursor
 * @client: DRM client
 */
#define drm_client_for_each_modeset(modeset, client) \
	for (({ lockdep_assert_held(&(client)->modeset_mutex); }), \
	     modeset = (client)->modesets; modeset->crtc; modeset++)

/**
 * drm_client_for_each_connector_iter - connector_list iterator macro
 * @connector: &struct drm_connector pointer used as cursor
 * @iter: &struct drm_connector_list_iter
 *
 * This iterates the connectors that are useable for internal clients (excludes
 * writeback connectors).
 *
 * For more info see drm_for_each_connector_iter().
 */
#define drm_client_for_each_connector_iter(connector, iter) \
	drm_for_each_connector_iter(connector, iter) \
		if (connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)

#endif
