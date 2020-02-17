/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "util.h"
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

struct render_data {
	pixman_region32_t *damage;
};

static void
render_texture(struct wlr_output *wlr_output, pixman_region32_t *output_damage, struct wlr_texture *texture,
	       const struct wlr_box *box, const float matrix[static 9])
{
	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_union_rect(&damage, &damage, box->x, box->y, box->width, box->height);
	pixman_region32_intersect(&damage, &damage, output_damage);
	if (!pixman_region32_not_empty(&damage)) {
		goto damage_finish;
	}

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		scissor_output(wlr_output, &rects[i]);
		wlr_render_texture_with_matrix(renderer, texture, matrix, 1.0f);
	}

damage_finish:
	pixman_region32_fini(&damage);
}

static void
render_surface_iterator(struct cg_output *output, struct wlr_surface *surface, struct wlr_box *box, void *user_data)
{
	struct render_data *data = user_data;
	struct wlr_output *wlr_output = output->wlr_output;
	pixman_region32_t *output_damage = data->damage;

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Cannot obtain surface texture");
		return;
	}

	scale_box(box, wlr_output->scale);

	float matrix[9];
	enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, box, transform, 0.0f, wlr_output->transform_matrix);

	render_texture(wlr_output, output_damage, texture, box, matrix);
}

static void
render_drag_icons(struct cg_output *output, pixman_region32_t *damage, struct wl_list *drag_icons)
{
	struct render_data data = {
		.damage = damage,
	};
	output_drag_icons_for_each_surface(output, drag_icons, render_surface_iterator, &data);
}

/**
 * Render all toplevels without descending into popups.
 */
static void
render_view_toplevels(struct cg_view *view, struct cg_output *output, pixman_region32_t *damage)
{
	struct render_data data = {
		.damage = damage,
	};
	double ox = view->lx;
	double oy = view->ly;
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &ox, &oy);
	output_surface_for_each_surface(output, view->wlr_surface, ox, oy, render_surface_iterator, &data);
}

static void
render_popup_iterator(struct cg_output *output, struct wlr_surface *surface, struct wlr_box *box, void *data)
{
	/* Render this popup's surface. */
	render_surface_iterator(output, surface, box, data);

	/* Render this popup's child toplevels. */
	output_surface_for_each_surface(output, surface, box->x, box->y, render_surface_iterator, data);
}

static void
render_view_popups(struct cg_view *view, struct cg_output *output, pixman_region32_t *damage)
{
	struct render_data data = {
		.damage = damage,
	};
	output_view_for_each_popup(output, view, render_popup_iterator, &data);
}

void
output_render(struct cg_output *output, pixman_region32_t *damage)
{
	struct cg_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);
	if (!renderer) {
		wlr_log(WLR_DEBUG, "Expected the output backend to have a renderer");
		return;
	}

	wlr_renderer_begin(renderer, wlr_output->width, wlr_output->height);

	if (!pixman_region32_not_empty(damage)) {
		wlr_log(WLR_DEBUG, "Output isn't damaged but needs a buffer swap");
		goto renderer_end;
	}

#ifdef DEBUG
	if (server->debug_damage_tracking) {
		wlr_renderer_clear(renderer, (float[]){1.0f, 0.0f, 0.0f, 1.0f});
	}
#endif

	float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		scissor_output(wlr_output, &rects[i]);
		wlr_renderer_clear(renderer, color);
	}

	// TODO: render only top view, possibly use focused view for this, see #35.
	struct cg_view *view;
	wl_list_for_each_reverse (view, &server->views, link) {
		render_view_toplevels(view, output, damage);
	}

	struct cg_view *focused_view = seat_get_focus(server->seat);
	if (focused_view) {
		render_view_popups(focused_view, output, damage);
	}

	render_drag_icons(output, damage, &server->seat->drag_icons);

renderer_end:
	/* Draw software cursor in case hardware cursors aren't
	   available. This is a no-op when they are. */
	wlr_output_render_software_cursors(wlr_output, damage);
	wlr_renderer_scissor(renderer, NULL);
	wlr_renderer_end(renderer);

	int output_width, output_height;
	wlr_output_transformed_resolution(wlr_output, &output_width, &output_height);

	pixman_region32_t frame_damage;
	pixman_region32_init(&frame_damage);

	enum wl_output_transform transform = wlr_output_transform_invert(wlr_output->transform);
	wlr_region_transform(&frame_damage, &output->damage->current, transform, output_width, output_height);

#ifdef DEBUG
	if (server->debug_damage_tracking) {
		pixman_region32_union_rect(&frame_damage, &frame_damage, 0, 0, output_width, output_height);
	}
#endif

	wlr_output_set_damage(wlr_output, &frame_damage);
	pixman_region32_fini(&frame_damage);

	if (!wlr_output_commit(wlr_output)) {
		wlr_log(WLR_ERROR, "Could not commit output");
	}
}
