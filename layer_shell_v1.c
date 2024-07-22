/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "layer_shell_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"

#include <stdlib.h>
#include <assert.h>
#include <complex.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define NUM_LAYERS (4)

static void
arrange_surface(struct cg_output *output, const struct wlr_box *full_area, struct wlr_box *usable_area,
		struct wlr_scene_tree *tree)
{
	struct wlr_scene_node *node;
	wl_list_for_each (node, &tree->children, link) {
		struct cg_view *view = node->data;
		if (!view) {
			continue;
		}
		struct cg_layer_surface *surface = (struct cg_layer_surface *) view;

		if (!surface->scene->layer_surface->initialized) {
			continue;
		}

		wlr_scene_layer_surface_v1_configure(surface->scene, full_area, usable_area);
	}
}

void
arrange_layers(struct cg_output *output)
{
	struct wlr_box usable_area = {0};
	wlr_output_effective_resolution(output->wlr_output, &usable_area.width, &usable_area.height);
	const struct wlr_box full_area = usable_area;

	arrange_surface(output, &full_area, &usable_area, output->layers.shell_background);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_bottom);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_top);
	arrange_surface(output, &full_area, &usable_area, output->layers.shell_overlay);

	if (!wlr_box_equal(&usable_area, &output->usable_area)) {
		wlr_log(WLR_DEBUG, "Usable area changed, rearranging output");
		output->usable_area = usable_area;
		// arrange_output(output);
	} else {
		// arrange_popups(root->layers.popup);
		// FIXME: popup is not implemented
	}
}

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

static void
handle_map(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *surface = wl_container_of(listener, surface, map);
	struct wlr_layer_surface_v1 *layer_surface = surface->scene->layer_surface;
	struct cg_server *server = surface->server;

	// focus on new surface
	if (layer_surface->current.keyboard_interactive &&
	    (layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
	     layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {
		struct cg_seat *seat = server->seat;
		if (!seat->focused_layer || seat->focused_layer->current.layer >= layer_surface->current.layer) {
			// seat_set_focus_layer(seat, layer_surface);
		}
		arrange_layers(surface->output);
	}
}

static void
handle_unmap(struct wl_listener *listener, void *data)
{
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *surface = wl_container_of(listener, surface, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = surface->scene->layer_surface;
	if (!layer_surface->initialized) {
		return;
	}

	uint32_t committed = layer_surface->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		enum zwlr_layer_shell_v1_layer layer_type = layer_surface->current.layer;
		struct wlr_scene_tree *output_layer = cg_layer_get_scene(surface->output, layer_type);
		wlr_scene_node_reparent(&surface->scene->tree->node, output_layer);
	}

	if (layer_surface->initial_commit || committed || layer_surface->surface->mapped != surface->mapped) {
		surface->mapped = layer_surface->surface->mapped;
		arrange_layers(surface->output);
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
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

	/*
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
	*/
	struct cg_output *output;
	if (layer_surface->output) {
		output = layer_surface->output->data;
	} else {
		struct wlr_output *wlr_output =
			wlr_output_layout_output_at(server->output_layout, seat->cursor->x, seat->cursor->y);
		layer_surface->output = wlr_output;
		output = wlr_output->data;
	}

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
	cg_surface->tree->node.data =cg_surface;
	cg_surface->layer_surface->data = cg_surface;
	cg_surface->output = output;

	cg_surface->map.notify = handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &cg_surface->map);
	cg_surface->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &cg_surface->unmap);
	cg_surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &cg_surface->surface_commit);

	cg_surface->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&layer_surface->surface->events.destroy, &cg_surface->output_destroy);
}
