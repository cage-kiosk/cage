/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_surface.h>

#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "renderer.h"
#include "util.h"
#include "view.h"

void
cage_output_damage_whole(struct cg_output *output)
{
	assert(output != NULL);

	wlr_output_damage_add_whole(output->damage);
}

void
cage_output_damage_region(struct cg_output *output, struct wlr_box *region)
{
	assert(output != NULL);
	assert(region != NULL);
	assert(output->wlr_output->enabled);

	scale_box(region, output->wlr_output->scale);
	wlr_output_damage_add_box(output->damage, region);
}

void
cage_output_damage_surface(struct cg_output *output, struct wlr_surface *surface, int sx, int sy)
{
	assert(output != NULL);
	assert(surface != NULL);
	struct wlr_output *wlr_output = output->wlr_output;

	assert(wlr_output->enabled);

	if (pixman_region32_not_empty(&surface->buffer_damage)) {
		pixman_region32_t damage;
		pixman_region32_init(&damage);
		wlr_surface_get_effective_damage(surface, &damage);

		wlr_region_scale(&damage, &damage, wlr_output->scale);
		if (ceil(wlr_output->scale) > surface->current.scale) {
			/* When scaling up a surface it'll becomevblurry, so we
			 * need to expand the damage region. */
			wlr_region_expand(&damage, &damage, ceil(wlr_output->scale) - surface->current.scale);
		}

		pixman_region32_translate(&damage, sx, sy);
		wlr_output_damage_add(output->damage, &damage);
		pixman_region32_fini(&damage);
	}
}

struct send_frame_done_data {
	struct timespec when;
};

static void
output_for_each_surface(struct cg_output *output, wlr_surface_iterator_func_t iterator, void *user_data)
{
	struct cg_view *view;
	wl_list_for_each_reverse (view, &output->views, link) {
		cage_view_for_each_surface(view, iterator, user_data);
	}
}

static void
send_frame_done_iterator(struct wlr_surface *surface, int sx, int sy, void *user_data)
{
	struct send_frame_done_data *data = user_data;
	wlr_surface_send_frame_done(surface, &data->when);
}

static void
send_frame_done(struct cg_output *output, struct send_frame_done_data *data)
{
	output_for_each_surface(output, send_frame_done_iterator, data);
}

static void
handle_output_damage_frame(struct wl_listener *listener, void *user_data)
{
	struct cg_output *output = wl_container_of(listener, output, damage_frame);
	struct send_frame_done_data frame_data = {0};

	bool needs_frame;
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	if (!wlr_output_damage_attach_render(output->damage, &needs_frame, &damage)) {
		wlr_log(WLR_ERROR, "Cannot make damage output current");
		goto damage_finish;
	}

	if (!needs_frame) {
		wlr_output_rollback(output->wlr_output);
		goto damage_finish;
	}

	cage_renderer_render_output(output, &damage);

damage_finish:
	pixman_region32_fini(&damage);

	clock_gettime(CLOCK_MONOTONIC, &frame_data.when);
	send_frame_done(output, &frame_data);
}

static void
handle_output_damage_destroy(struct wl_listener *listener, void *user_data)
{
	struct cg_output *output = wl_container_of(listener, output, damage_destroy);

	if (output->wlr_output->enabled) {
		cage_output_disable(output);
	}

	wl_list_remove(&output->damage_destroy.link);
}

static void
handle_output_transform(struct wl_listener *listener, void *user_data)
{
	struct cg_output *output = wl_container_of(listener, output, transform);

	assert(!output->wlr_output->enabled);

	struct cg_view *view;
	wl_list_for_each (view, &output->views, link) {
		cage_view_position(view);
	}
}

static void
handle_output_mode(struct wl_listener *listener, void *user_data)
{
	struct cg_output *output = wl_container_of(listener, output, mode);

	assert(!output->wlr_output->enabled);

	struct cg_view *view;
	wl_list_for_each (view, &output->views, link) {
		cage_view_position(view);
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *user_data)
{
	struct cg_output *output = wl_container_of(listener, output, destroy);
	cage_output_fini(output);
}

void
cage_output_get_geometry(struct cg_output *output, struct wlr_box *geometry)
{
	assert(output != NULL);
	assert(geometry != NULL);

	wlr_output_effective_resolution(output->wlr_output, &geometry->width, &geometry->height);
}

void
cage_output_disable(struct cg_output *output)
{
	assert(output != NULL);
	assert(output->wlr_output->enabled);

	struct wlr_output *wlr_output = output->wlr_output;

	wlr_log(WLR_DEBUG, "Disabling output %s", wlr_output->name);

	wl_list_remove(&output->mode.link);
	wl_list_init(&output->mode.link);
	wl_list_remove(&output->transform.link);
	wl_list_init(&output->transform.link);
	wl_list_remove(&output->damage_frame.link);
	wl_list_init(&output->damage_frame.link);

	wlr_output_rollback(wlr_output);
	wlr_output_enable(wlr_output, false);
	wlr_output_commit(wlr_output);
}

void
cage_output_enable(struct cg_output *output)
{
	assert(output != NULL);
	/* Outputs get enabled by the backend before firing the new_output event,
	 * so we can't do a check for already enabled outputs here unless we
	 * duplicate the enabled property in cg_output. */

	struct wlr_output *wlr_output = output->wlr_output;

	wlr_log(WLR_DEBUG, "Enabling output %s", wlr_output->name);

	wl_list_remove(&output->mode.link);
	output->mode.notify = handle_output_mode;
	wl_signal_add(&wlr_output->events.mode, &output->mode);
	wl_list_remove(&output->transform.link);
	output->transform.notify = handle_output_transform;
	wl_signal_add(&wlr_output->events.transform, &output->transform);
	wl_list_remove(&output->damage_frame.link);
	output->damage_frame.notify = handle_output_damage_frame;
	wl_signal_add(&output->damage->events.frame, &output->damage_frame);

	wlr_output_enable(wlr_output, true);
	wlr_output_commit(wlr_output);
	cage_output_damage_whole(output);
}

void
cage_output_init(struct cg_output *output, struct wlr_output *wlr_output)
{
	assert(output != NULL);
	assert(wlr_output != NULL);

	output->wlr_output = wlr_output;
	wlr_output->data = output;
	output->damage = wlr_output_damage_create(wlr_output);
	wl_list_init(&output->views);

	/* We need to init the lists here because cage_output_enable calls
	 * `wl_list_remove` on these. */
	wl_list_init(&output->mode.link);
	wl_list_init(&output->transform.link);
	wl_list_init(&output->damage_frame.link);

	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->damage_destroy.notify = handle_output_damage_destroy;
	wl_signal_add(&output->damage->events.destroy, &output->damage_destroy);

	struct wlr_output_mode *preferred_mode = wlr_output_preferred_mode(wlr_output);
	if (preferred_mode) {
		wlr_output_set_mode(wlr_output, preferred_mode);
	}

	cage_output_enable(output);
}

void
cage_output_fini(struct cg_output *output)
{
	assert(output != NULL);

	if (output->wlr_output->enabled) {
		cage_output_disable(output);
	}

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	free(output);
}

void
cage_output_set_window_title(struct cg_output *output, const char *title)
{
	assert(output != NULL);
	assert(title != NULL);
	assert(output->wlr_output->enabled);

	struct wlr_output *wlr_output = output->wlr_output;

	if (wlr_output_is_wl(wlr_output)) {
		wlr_wl_output_set_title(wlr_output, title);
#if WLR_HAS_X11_BACKEND
	} else if (wlr_output_is_x11(wlr_output)) {
		wlr_x11_output_set_title(wlr_output, title);
#endif
	}
}
