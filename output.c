/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"
#include <wlr/config.h>

#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
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
#include "server.h"
#include "view.h"

static void
scissor_output(struct wlr_output *output, pixman_box32_t *rect)
{
	struct wlr_renderer *renderer = wlr_backend_get_renderer(output->backend);

	struct wlr_box box = {
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height = rect->y2 - rect->y1,
	};

	int output_width, output_height;
	wlr_output_transformed_resolution(output, &output_width, &output_height);
	enum wl_output_transform transform = wlr_output_transform_invert(output->transform);
	wlr_box_transform(&box, &box, transform, output_width, output_height);

	wlr_renderer_scissor(renderer, &box);
}

static void
send_frame_done(struct wlr_surface *surface, int _unused, int _not_used, void *data)
{
	struct timespec *now = data;
	wlr_surface_send_frame_done(surface, now);
}

/* Used to move all of the data necessary to damage a surface. */
struct damage_data {
	struct cg_output *output;

	/* Output-local coordinates. */
	double ox, oy;
	bool whole;
};

static void
damage_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct damage_data *ddata = data;
	struct cg_output *output = ddata->output;
	struct wlr_output *wlr_output = output->wlr_output;

	if (!wlr_surface_has_buffer(surface)) {
		return;
	}

	struct wlr_box box = {
		.x = (ddata->ox + sx) * wlr_output->scale,
		.y = (ddata->oy + sy) * wlr_output->scale,
		.width = surface->current.width * wlr_output->scale,
		.height = surface->current.height * wlr_output->scale,
	};

	if (ddata->whole) {
		wlr_output_damage_add_box(output->damage, &box);
	} else if (pixman_region32_not_empty(&surface->buffer_damage)) {
		pixman_region32_t damage;
		pixman_region32_init(&damage);
		wlr_surface_get_effective_damage(surface, &damage);

		wlr_region_scale(&damage, &damage, wlr_output->scale);
		if (ceil(wlr_output->scale) > surface->current.scale) {
			/* When scaling up a surface it'll become
			   blurry, so we need to expand the damage
			   region. */
			wlr_region_expand(&damage, &damage,
					  ceil(wlr_output->scale) - surface->current.scale);
		}
		pixman_region32_translate(&damage, box.x, box.y);
		wlr_output_damage_add(output->damage, &damage);
		pixman_region32_fini(&damage);
	}
}

/* Used to move all of the data necessary to render a surface from the
 * top-level frame handler to the per-surface render function. */
struct render_data {
	struct wlr_output_layout *output_layout;
	struct wlr_output *output;
	struct timespec *when;
	pixman_region32_t *damage;

	/* Output-local coordinates. */
	double ox, oy;
};

static void
render_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct render_data *rdata = data;
	struct wlr_output *output = rdata->output;

	if (!wlr_surface_has_buffer(surface)) {
		return;
	}

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Cannot obtain surface texture");
		return;
	}

	struct wlr_box box = {
		.x = (rdata->ox + sx) * output->scale,
		.y = (rdata->oy + sy) * output->scale,
		.width = surface->current.width * output->scale,
		.height = surface->current.height * output->scale,
	};

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_union_rect(&damage, &damage, box.x, box.y, box.width, box.height);
	pixman_region32_intersect(&damage, &damage, rdata->damage);
	if (!pixman_region32_not_empty(&damage)) {
		goto buffer_damage_finish;
	}

	float matrix[9];
	enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0, output->transform_matrix);

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		scissor_output(output, &rects[i]);
		wlr_render_texture_with_matrix(surface->renderer, texture, matrix, 1);
	}

 buffer_damage_finish:
	pixman_region32_fini(&damage);
}

static void
drag_icons_for_each_surface(struct cg_server *server, wlr_surface_iterator_func_t iterator,
			    void *data)
{
	struct render_data *rdata = data;
	struct wlr_output *wlr_output = rdata->output;

	struct cg_drag_icon *drag_icon;
	wl_list_for_each(drag_icon, &server->seat->drag_icons, link) {
		if (!drag_icon->wlr_drag_icon->mapped) {
			continue;
		}
		rdata->ox = drag_icon->x;
		rdata->oy = drag_icon->y;
		wlr_output_layout_output_coords(server->output_layout, wlr_output, &rdata->ox, &rdata->oy);
		wlr_surface_for_each_surface(drag_icon->wlr_drag_icon->surface,
					     iterator,
					     data);
	}
}

static void
handle_output_damage_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, damage_frame);
	struct wlr_renderer *renderer = wlr_backend_get_renderer(output->server->backend);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	bool needs_frame;
	pixman_region32_t buffer_damage;
	pixman_region32_init(&buffer_damage);
	if (!wlr_output_damage_attach_render(output->damage, &needs_frame, &buffer_damage)) {
		wlr_log(WLR_ERROR, "Cannot make damage output current");
		goto buffer_damage_finish;
	}

	if (!needs_frame) {
		goto buffer_damage_finish;
	}

	wlr_renderer_begin(renderer, output->wlr_output->width, output->wlr_output->height);

	if (!pixman_region32_not_empty(&buffer_damage)) {
		wlr_log(WLR_DEBUG, "Output isn't damaged but needs a buffer frame");
		goto renderer_end;
	}

#ifdef DEBUG
	if (output->server->debug_damage_tracking) {
		wlr_renderer_clear(renderer, (float[]){1, 0, 0, 1});
	}
#endif

	float color[4] = {0, 0, 0, 1.0};
	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&buffer_damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		scissor_output(output->wlr_output, &rects[i]);
		wlr_renderer_clear(renderer, color);
	}

	struct render_data rdata = {
		.output_layout = output->server->output_layout,
		.output = output->wlr_output,
		.when = &now,
		.damage = &buffer_damage,
	};

	struct cg_view *view;
	wl_list_for_each_reverse(view, &output->server->views, link) {
		rdata.ox = view->lx;
		rdata.oy = view->ly;
		wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &rdata.ox, &rdata.oy);
		view_for_each_surface(view, render_surface, &rdata);
	}

	drag_icons_for_each_surface(output->server, render_surface, &rdata);

 renderer_end:
	/* Draw software cursor in case hardware cursors aren't
	   available. This is a no-op when they are. */
	wlr_output_render_software_cursors(output->wlr_output, &buffer_damage);
	wlr_renderer_scissor(renderer, NULL);
	wlr_renderer_end(renderer);

	int output_width, output_height;
	wlr_output_transformed_resolution(output->wlr_output, &output_width, &output_height);

	pixman_region32_t frame_damage;
	pixman_region32_init(&frame_damage);

	enum wl_output_transform transform = wlr_output_transform_invert(output->wlr_output->transform);
	wlr_region_transform(&frame_damage, &output->damage->current, transform, output_width, output_height);

#ifdef DEBUG
	if (output->server->debug_damage_tracking) {
		pixman_region32_union_rect(&frame_damage, &frame_damage, 0, 0, output_width, output_height);
	}
#endif

	wlr_output_set_damage(output->wlr_output, &frame_damage);
	pixman_region32_fini(&frame_damage);

	if (!wlr_output_commit(output->wlr_output)) {
	        wlr_log(WLR_ERROR, "Could not commit output");
		goto buffer_damage_finish;
	}

 buffer_damage_finish:
	pixman_region32_fini(&buffer_damage);

	wl_list_for_each_reverse(view, &output->server->views, link) {
		view_for_each_surface(view, send_frame_done, &now);
	}
	drag_icons_for_each_surface(output->server, send_frame_done, &now);
}

static void
handle_output_transform(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, transform);

	struct cg_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		view_position(view);
	}
}

static void
handle_output_mode(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, mode);

	struct cg_view *view;
	wl_list_for_each(view, &output->server->views, link) {
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
	wl_list_for_each(view, &output->server->views, link) {
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
	struct wlr_output_mode *preferred_mode;

	preferred_mode = wlr_output_preferred_mode(wlr_output);
	if (preferred_mode) {
		wlr_output_set_mode(wlr_output, preferred_mode);
	}

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

	struct cg_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		view_position(view);
	}

	wlr_output_set_transform(wlr_output, server->output_transform);

	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	if (wlr_xcursor_manager_load(server->seat->xcursor_manager, wlr_output->scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for output '%s' with scale %f",
			wlr_output->name,
			wlr_output->scale);
	}

	/* Place the cursor in the center of the screen. */
	wlr_cursor_warp(server->seat->cursor, NULL, wlr_output->width / 2, wlr_output->height / 2);
	wlr_output_damage_add_whole(output->damage);
}

void
output_damage_view_surface(struct cg_output *output, struct cg_view *view)
{
	struct damage_data data = {
		.output = output,
		.ox = view->lx,
		.oy = view->ly,
		.whole = false,
	};
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &data.ox, &data.oy);
	view_for_each_surface(view, damage_surface, &data);
}

void
output_damage_view_whole(struct cg_output *output, struct cg_view *view)
{
	struct damage_data data = {
		.output = output,
		.ox = view->lx,
		.oy = view->ly,
		.whole = true,
	};
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &data.ox, &data.oy);
	view_for_each_surface(view, damage_surface, &data);
}

void
output_damage_drag_icon(struct cg_output *output, struct cg_drag_icon *drag_icon)
{
	struct damage_data data = {
		.output = output,
		.ox = drag_icon->x,
		.oy = drag_icon->y,
		.whole = true,
	};
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &data.ox, &data.oy);
	wlr_surface_for_each_surface(drag_icon->wlr_drag_icon->surface,
				     damage_surface,
				     &data);
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
