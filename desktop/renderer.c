/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "renderer.h"
#include "util.h"
#include "view.h"

struct render_data {
	struct wlr_output *wlr_output;
	pixman_region32_t *damage;
};

static void
renderer_scissor(struct wlr_output *wlr_output, pixman_box32_t *rect)
{
	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);

	struct wlr_box box = {
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height = rect->y2 - rect->y1,
	};

	int output_width, output_height;
	wlr_output_transformed_resolution(wlr_output, &output_width, &output_height);
	enum wl_output_transform transform = wlr_output_transform_invert(wlr_output->transform);
	wlr_box_transform(&box, &box, transform, output_width, output_height);

	wlr_renderer_scissor(renderer, &box);
}

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
		renderer_scissor(wlr_output, &rects[i]);
		wlr_render_texture_with_matrix(renderer, texture, matrix, 1.0f);
	}

damage_finish:
	pixman_region32_fini(&damage);
}

static void
render_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *user_data)
{
	struct render_data *data = user_data;

	assert(data->wlr_output);
	assert(data->damage);

	struct wlr_output *wlr_output = data->wlr_output;
	pixman_region32_t *output_damage = data->damage;

	struct wlr_box geometry = {0};
	wlr_output_transformed_resolution(wlr_output, &geometry.width, &geometry.height);

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Cannot obtain surface texture");
		return;
	}

	scale_box(&geometry, wlr_output->scale);

	float matrix[9];
	enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &geometry, transform, 0.0f, wlr_output->transform_matrix);

	render_texture(wlr_output, output_damage, texture, &geometry, matrix);
}

static void
clear_output(struct wlr_output *wlr_output, pixman_region32_t *damage)
{
	static float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);

#ifdef DEBUG_DAMAGE_TRACKING
	wlr_renderer_clear(renderer, (float[]){1.0f, 0.0f, 0.0f, 1.0f});
#endif

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		renderer_scissor(wlr_output, &rects[i]);
		wlr_renderer_clear(renderer, color);
	}
}

static void
render_view(struct cg_view *view, struct wlr_output *wlr_output, pixman_region32_t *damage)
{
	struct render_data data = {
		.wlr_output = wlr_output,
		.damage = damage,
	};
	cage_view_for_each_surface(view, render_surface_iterator, &data);
}

static void
renderer_end(struct cg_output *output, pixman_region32_t *damage)
{
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);

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

#ifdef DEBUG_DAMAGE_TRACKING
	pixman_region32_union_rect(&frame_damage, &frame_damage, 0, 0, output_width, output_height);
#endif

	wlr_output_set_damage(wlr_output, &frame_damage);
	pixman_region32_fini(&frame_damage);

	if (!wlr_output_commit(wlr_output)) {
		wlr_log(WLR_ERROR, "Could not commit output");
	}
}

void
cage_renderer_render_output(struct cg_output *output, pixman_region32_t *damage)
{
	assert(output != NULL);
	assert(damage != NULL);
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_renderer *renderer = wlr_backend_get_renderer(wlr_output->backend);

	wlr_renderer_begin(renderer, wlr_output->width, wlr_output->height);

	if (!pixman_region32_not_empty(damage)) {
		wlr_log(WLR_DEBUG, "Output isn't damaged but needs a buffer swap");
		goto renderer_end;
	}

	clear_output(wlr_output, damage);

	// TODO: render only top view, possibly use focused view for this, see #35.
	struct cg_view *view;
	wl_list_for_each_reverse (view, &output->views, link) {
		render_view(view, wlr_output, damage);
	}

renderer_end:
	renderer_end(output, damage);
}
