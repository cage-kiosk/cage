/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2021 Jente Hidskes
 * Copyright (C) 2026 Sungjoon Moon
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "layer_shell_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"

static struct wlr_scene_tree *
layer_get_scene(struct cg_output *output, enum zwlr_layer_shell_v1_layer type)
{
	switch (type) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return output->layers.shell_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return output->layers.shell_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return output->layers.shell_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return output->layers.shell_overlay;
	}

	/* wlroots rejects unknown layers. */
	abort();
}

static void
popup_unconstrain(struct cg_layer_popup *popup)
{
	struct cg_output *output = popup->layer->output;

	/* The output may be going away. */
	if (!output) {
		return;
	}

	int lx, ly;
	wlr_scene_node_coords(&popup->layer->tree->node, &lx, &ly);

	struct wlr_box output_box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &output_box);

	/* The output box in the coordinate system of the layer surface. */
	struct wlr_box box = {
		.x = output_box.x - lx,
		.y = output_box.y - ly,
		.width = output_box.width,
		.height = output_box.height,
	};

	wlr_xdg_popup_unconstrain_from_box(popup->wlr_popup, &box);
}

static void
popup_handle_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->wlr_popup->base->initial_commit) {
		popup_unconstrain(popup);
	}
}

static void
popup_handle_reposition(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

static void
popup_handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	free(popup);
}

static void popup_handle_new_popup(struct wl_listener *listener, void *data);

static void
popup_create(struct wlr_xdg_popup *wlr_popup, struct cg_layer_surface *layer, struct wlr_scene_tree *parent)
{
	struct cg_layer_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		return;
	}

	popup->layer = layer;
	popup->wlr_popup = wlr_popup;
	popup->scene = wlr_scene_xdg_surface_create(parent, wlr_popup->base);
	if (!popup->scene) {
		free(popup);
		return;
	}

	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);
	popup->reposition.notify = popup_handle_reposition;
	wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);
}

static void
popup_handle_new_popup(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, new_popup);
	popup_create(data, popup->layer, popup->scene);
}

static void
handle_new_popup(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, new_popup);
	popup_create(data, layer, layer->popups);
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	uint32_t committed = layer_surface->current.committed;

	if (layer_surface->initialized && committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *tree = layer_get_scene(layer->output, layer_surface->current.layer);
		wlr_scene_node_reparent(&layer->tree->node, tree);
	}

	if (layer_surface->initial_commit || committed) {
		arrange_layers(layer->output);
	}
}

static void
handle_map(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, map);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	struct cg_seat *seat = layer->server->seat;

	if (layer_surface->current.keyboard_interactive &&
	    layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
	    (!seat->focused_layer || seat->focused_layer->current.layer <= layer_surface->current.layer)) {
		seat_set_focus_layer(seat, layer_surface);
	}

	arrange_layers(layer->output);
}

static void
handle_unmap(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, unmap);
	struct cg_seat *seat = layer->server->seat;

	/* wlroots destroys popups only after this signal, but their grab would swallow the focus change below. */
	struct wlr_xdg_popup *popup, *tmp;
	wl_list_for_each_safe (popup, tmp, &layer->layer_surface->popups, link) {
		wlr_xdg_popup_destroy(popup);
	}

	/* Runs from output_destroy_layers() too, with output already NULL. */
	if (seat->focused_layer == layer->layer_surface) {
		seat_set_focus_layer(seat, NULL);
	}

	if (layer->output) {
		arrange_layers(layer->output);
	}
}

static void
handle_node_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, node_destroy);

	/* The node is still linked in its layer tree; hide it from arrange_layers(). */
	layer->tree->node.data = NULL;
	wl_list_remove(&layer->link);

	if (layer->output) {
		arrange_layers(layer->output);
	}

	wlr_scene_node_destroy(&layer->popups->node);

	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->surface_commit.link);
	wl_list_remove(&layer->node_destroy.link);
	wl_list_remove(&layer->new_popup.link);

	layer->layer_surface->data = NULL;
	free(layer);
}

void
handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_layer_shell_v1_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	wlr_log(WLR_DEBUG, "New layer surface: namespace %s layer %d anchor %d size %dx%d margin %d,%d,%d,%d",
		layer_surface->namespace, layer_surface->pending.layer, layer_surface->pending.anchor,
		layer_surface->pending.desired_width, layer_surface->pending.desired_height,
		layer_surface->pending.margin.top, layer_surface->pending.margin.right,
		layer_surface->pending.margin.bottom, layer_surface->pending.margin.left);

	if (!layer_surface->output) {
		struct cg_output *output;
		wl_list_for_each (output, &server->outputs, link) {
			if (wlr_output_layout_get(server->output_layout, output->wlr_output)) {
				layer_surface->output = output->wlr_output;
				break;
			}
		}
		if (!layer_surface->output) {
			wlr_log(WLR_ERROR, "No output to assign layer surface '%s' to", layer_surface->namespace);
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
	}

	struct cg_output *output = layer_surface->output->data;
	if (!output || !wlr_output_layout_get(server->output_layout, layer_surface->output)) {
		wlr_log(WLR_ERROR, "Layer surface '%s' requested an output that is not in use",
			layer_surface->namespace);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct cg_layer_surface *layer = calloc(1, sizeof(*layer));
	if (!layer) {
		wlr_log(WLR_ERROR, "Could not allocate a layer surface");
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer->scene =
		wlr_scene_layer_surface_v1_create(layer_get_scene(output, layer_surface->pending.layer), layer_surface);
	layer->popups = wlr_scene_tree_create(server->layers.popup);
	if (!layer->scene || !layer->popups) {
		wlr_log(WLR_ERROR, "Could not allocate a layer surface scene node");
		if (layer->popups) {
			wlr_scene_node_destroy(&layer->popups->node);
		}
		free(layer);
		/* Also destroys layer->scene. */
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer->tree = layer->scene->tree;
	layer->server = server;
	layer->output = output;
	layer->layer_surface = layer_surface;
	layer_surface->data = layer;
	layer->tree->node.data = layer;
	wl_list_insert(&output->layer_surfaces, &layer->link);

	wlr_fractional_scale_v1_notify_scale(layer_surface->surface, layer_surface->output->scale);
	wlr_surface_set_preferred_buffer_scale(layer_surface->surface, ceil(layer_surface->output->scale));

	layer->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->surface_commit);
	layer->map.notify = handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);
	layer->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
	layer->new_popup.notify = handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);
	layer->node_destroy.notify = handle_node_destroy;
	wl_signal_add(&layer->tree->node.events.destroy, &layer->node_destroy);
}
