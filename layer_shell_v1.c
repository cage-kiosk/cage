/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "layer_shell_v1.h"
#include "output.h"
#include "server.h"

#include <assert.h>
#include <complex.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

static struct wlr_scene_tree *
cg_layer_get_scene(struct cg_output *output, enum zwlr_layer_shell_v1_layer layer_type)
{
	assert(layer_type <= NUM_LAYERS);
	switch (layer_type) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return output->layers.shell_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return output->layers.shell_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return output->layers.shell_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return output->layers.shell_overlay;
	}
	return NULL;
}

void
handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_layer_shell_v1_surface);
	struct cg_seat *seat = server->seat;
	struct wlr_layer_surface_v1 *layer_surface = data;

	wlr_log(WLR_DEBUG,
		"New layer shell surface: namespace %s layer %d anchor %" PRIu32 " size %" PRIu32 "x%" PRIu32
		" margin %" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",",
		layer_surface->namespace, layer_surface->pending.layer, layer_surface->pending.anchor,
		layer_surface->pending.desired_width, layer_surface->pending.desired_height,
		layer_surface->pending.margin.top, layer_surface->pending.margin.right,
		layer_surface->pending.margin.bottom, layer_surface->pending.margin.left);

	if (layer_surface->output) {
		struct wlr_output *wlr_output =
			wlr_output_layout_output_at(server->output_layout, seat->cursor->x, seat->cursor->y);
		if (wlr_output) {
			layer_surface->output = wlr_output;
		} else {
			struct cg_output *output = wl_container_of(server->outputs.prev, output, link);
			layer_surface->output = output->wlr_output;
		}
	}
	struct cg_output *output = layer_surface->output->data;

	enum zwlr_layer_shell_v1_layer layer_type = layer_surface->pending.layer;
	struct wlr_scene_tree *output_layer = cg_layer_get_scene(output, layer_type);

	struct cg_layer_surface *cg_surface = calloc(1, sizeof(*cg_surface));
	if (!cg_surface) {
		wlr_layer_surface_v1_destroy(layer_surface);
		wlr_log(WLR_ERROR, "Failed to allocate layer shell");
		return;
	}

	struct wlr_scene_layer_surface_v1 *scene = wlr_scene_layer_surface_v1_create(output_layer, layer_surface);
	if (!scene) {
		wlr_log(WLR_ERROR, "Could not allocate a layer_surface_v1");
		return;
	}

	cg_surface->server = server;
	cg_surface->layer_surface = scene->layer_surface;
	cg_surface->scene = scene;
	cg_surface->tree = scene->tree;
	cg_surface->layer_surface->data = cg_surface;
	cg_surface->output = output;

	cg_surface->map.notify = handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &cg_surface->map);
	cg_surface->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &cg_surface->unmap);
	cg_surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &cg_surface->surface_commit);

	cg_surface->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->events.disable, &surface->output_destroy);
}
