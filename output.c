/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "render.h"
#include "seat.h"
#include "server.h"
#include "util.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

static void output_for_each_surface(struct cg_output *output, cg_surface_iterator_func_t iterator, void *user_data);

struct surface_iterator_data {
	cg_surface_iterator_func_t user_iterator;
	void *user_data;

	struct cg_output *output;

	/* Output-local coordinates. */
	double ox, oy;
};

static bool
intersects_with_output(struct cg_output *output, struct wlr_output_layout *output_layout, struct wlr_box *surface_box)
{
	/* Since the surface_box's x- and y-coordinates are already output local,
	 * the x- and y-coordinates of this box need to be 0 for this function to
	 * work correctly. */
	struct wlr_box output_box = {0};
	wlr_output_effective_resolution(output->wlr_output, &output_box.width, &output_box.height);

	struct wlr_box intersection;
	return wlr_box_intersection(&intersection, &output_box, surface_box);
}

static void
output_for_each_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *user_data)
{
	struct surface_iterator_data *data = user_data;
	struct cg_output *output = data->output;

	if (!wlr_surface_has_buffer(surface)) {
		return;
	}

	struct wlr_box surface_box = {
		.x = data->ox + sx + surface->sx,
		.y = data->oy + sy + surface->sy,
		.width = surface->current.width,
		.height = surface->current.height,
	};

	if (!intersects_with_output(output, output->server->output_layout, &surface_box)) {
		return;
	}

	data->user_iterator(data->output, surface, &surface_box, data->user_data);
}

void
output_surface_for_each_surface(struct cg_output *output, struct wlr_surface *surface, double ox, double oy,
				cg_surface_iterator_func_t iterator, void *user_data)
{
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = ox,
		.oy = oy,
	};

	wlr_surface_for_each_surface(surface, output_for_each_surface_iterator, &data);
}

static void
output_view_for_each_surface(struct cg_output *output, struct cg_view *view, cg_surface_iterator_func_t iterator,
			     void *user_data)
{
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = view->lx,
		.oy = view->ly,
	};

	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &data.ox, &data.oy);
	view_for_each_surface(view, output_for_each_surface_iterator, &data);
}

void
output_view_for_each_popup(struct cg_output *output, struct cg_view *view, cg_surface_iterator_func_t iterator,
			   void *user_data)
{
	struct surface_iterator_data data = {
		.user_iterator = iterator,
		.user_data = user_data,
		.output = output,
		.ox = view->lx,
		.oy = view->ly,
	};

	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &data.ox, &data.oy);
	view_for_each_popup(view, output_for_each_surface_iterator, &data);
}

void
output_drag_icons_for_each_surface(struct cg_output *output, struct wl_list *drag_icons,
				   cg_surface_iterator_func_t iterator, void *user_data)
{
	struct cg_drag_icon *drag_icon;
	wl_list_for_each (drag_icon, drag_icons, link) {
		if (drag_icon->wlr_drag_icon->mapped) {
			double ox = drag_icon->lx;
			double oy = drag_icon->ly;
			wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &ox, &oy);
			output_surface_for_each_surface(output, drag_icon->wlr_drag_icon->surface, ox, oy, iterator,
							user_data);
		}
	}
}

static void
output_for_each_surface(struct cg_output *output, cg_surface_iterator_func_t iterator, void *user_data)
{
	struct cg_view *view;
	wl_list_for_each_reverse (view, &output->server->views, link) {
		output_view_for_each_surface(output, view, iterator, user_data);
	}

	output_drag_icons_for_each_surface(output, &output->server->seat->drag_icons, iterator, user_data);
}

struct send_frame_done_data {
	struct timespec when;
};

static void
send_frame_done_iterator(struct cg_output *output, struct wlr_surface *surface, struct wlr_box *box, void *user_data)
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
count_surface_iterator(struct cg_output *output, struct wlr_surface *surface, struct wlr_box *_box, void *data)
{
	size_t *n = data;
	n++;
}

static bool
scan_out_primary_view(struct cg_output *output)
{
	struct cg_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	struct cg_drag_icon *drag_icon;
	wl_list_for_each (drag_icon, &server->seat->drag_icons, link) {
		if (drag_icon->wlr_drag_icon->mapped) {
			return false;
		}
	}

	struct cg_view *view = seat_get_focus(server->seat);
	if (!view || !view->wlr_surface) {
		return false;
	}

	size_t n_surfaces = 0;
	output_view_for_each_surface(output, view, count_surface_iterator, &n_surfaces);
	if (n_surfaces > 1) {
		return false;
	}

#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW) {
		struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
		if (!wl_list_empty(&xwayland_view->xwayland_surface->children)) {
			return false;
		}
	}
#endif

	struct wlr_surface *surface = view->wlr_surface;

	if (!surface->buffer) {
		return false;
	}

	if ((float) surface->current.scale != wlr_output->scale ||
	    surface->current.transform != wlr_output->transform) {
		return false;
	}

	if (!wlr_output_attach_buffer(wlr_output, surface->buffer)) {
		return false;
	}

	return wlr_output_commit(wlr_output);
}

static void
damage_surface_iterator(struct cg_output *output, struct wlr_surface *surface, struct wlr_box *box, void *user_data)
{
	struct wlr_output *wlr_output = output->wlr_output;
	bool whole = *(bool *) user_data;

	scale_box(box, output->wlr_output->scale);

	if (whole) {
		wlr_output_damage_add_box(output->damage, box);
	} else if (pixman_region32_not_empty(&surface->buffer_damage)) {
		pixman_region32_t damage;
		pixman_region32_init(&damage);
		wlr_surface_get_effective_damage(surface, &damage);

		wlr_region_scale(&damage, &damage, wlr_output->scale);
		if (ceil(wlr_output->scale) > surface->current.scale) {
			/* When scaling up a surface it'll become
			   blurry, so we need to expand the damage
			   region. */
			wlr_region_expand(&damage, &damage, ceil(wlr_output->scale) - surface->current.scale);
		}
		pixman_region32_translate(&damage, box->x, box->y);
		wlr_output_damage_add(output->damage, &damage);
		pixman_region32_fini(&damage);
	}
}

void
output_damage_surface(struct cg_output *output, struct wlr_surface *surface, double lx, double ly, bool whole)
{
	double ox = lx, oy = ly;
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &ox, &oy);
	output_surface_for_each_surface(output, surface, ox, oy, damage_surface_iterator, &whole);
}

static void
handle_output_damage_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, damage_frame);
	struct send_frame_done_data frame_data = {0};

	if (!output->wlr_output->enabled) {
		return;
	}

	/* Check if we can scan-out the primary view. */
	static bool last_scanned_out = false;
	bool scanned_out = scan_out_primary_view(output);

	if (scanned_out && !last_scanned_out) {
		wlr_log(WLR_DEBUG, "Scanning out primary view");
	}
	if (last_scanned_out && !scanned_out) {
		wlr_log(WLR_DEBUG, "Stopping primary view scan out");
	}
	last_scanned_out = scanned_out;

	if (scanned_out) {
		goto frame_done;
	}

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

	output_render(output, &damage);

damage_finish:
	pixman_region32_fini(&damage);

frame_done:
	clock_gettime(CLOCK_MONOTONIC, &frame_data.when);
	send_frame_done(output, &frame_data);
}

static void
handle_output_transform(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, transform);

	if (!output->wlr_output->enabled) {
		return;
	}

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}
}

static void
handle_output_mode(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, mode);

	if (!output->wlr_output->enabled) {
		return;
	}

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}
}

static void
output_destroy(struct cg_output *output)
{
	struct cg_server *server = output->server;

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->mode.link);
	wl_list_remove(&output->transform.link);
	wl_list_remove(&output->damage_frame.link);
	wl_list_remove(&output->damage_destroy.link);
	wl_list_remove(&output->link);

	wlr_output_layout_remove(server->output_layout, output->wlr_output);

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}

	free(output);

	if (wl_list_empty(&server->outputs)) {
		wl_display_terminate(server->wl_display);
	}
}

static void
handle_output_damage_destroy(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, damage_destroy);
	output_destroy(output);
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, destroy);
	wlr_output_damage_destroy(output->damage);
	output_destroy(output);
}

void
handle_new_output(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	struct cg_output *output = calloc(1, sizeof(struct cg_output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output");
		return;
	}

	output->wlr_output = wlr_output;
	output->server = server;
	output->damage = wlr_output_damage_create(wlr_output);
	wl_list_insert(&server->outputs, &output->link);

	output->mode.notify = handle_output_mode;
	wl_signal_add(&wlr_output->events.mode, &output->mode);
	output->transform.notify = handle_output_transform;
	wl_signal_add(&wlr_output->events.transform, &output->transform);
	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->damage_frame.notify = handle_output_damage_frame;
	wl_signal_add(&output->damage->events.frame, &output->damage_frame);
	output->damage_destroy.notify = handle_output_damage_destroy;
	wl_signal_add(&output->damage->events.destroy, &output->damage_destroy);

	struct wlr_output_mode *preferred_mode = wlr_output_preferred_mode(wlr_output);
	if (preferred_mode) {
		wlr_output_set_mode(wlr_output, preferred_mode);
	}
	wlr_output_set_transform(wlr_output, server->output_transform);
	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}

	if (wlr_xcursor_manager_load(server->seat->xcursor_manager, wlr_output->scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for output '%s' with scale %f", wlr_output->name,
			wlr_output->scale);
	}

	wlr_output_enable(wlr_output, true);
	wlr_output_commit(wlr_output);

	wlr_output_damage_add_whole(output->damage);
}

void
output_set_window_title(struct cg_output *output, const char *title)
{
	struct wlr_output *wlr_output = output->wlr_output;

	if (wlr_output_is_wl(wlr_output)) {
		wlr_wl_output_set_title(wlr_output, title);
#if WLR_HAS_X11_BACKEND
	} else if (wlr_output_is_x11(wlr_output)) {
		wlr_x11_output_set_title(wlr_output, title);
#endif
	}
}
