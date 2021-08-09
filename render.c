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
#include <wlr/types/wlr_scene.h>
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

	double lx = 0, ly = 0;
	wlr_output_layout_output_coords(output->server->output_layout, output->wlr_output, &lx, &ly);
	wlr_scene_render_output(server->scene, wlr_output, lx, ly, damage);

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
