/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2021 Jente Hidskes
 * Copyright (C) 2026 Sungjoon Moon
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "layer_shell_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"

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

static struct wlr_scene_tree *
get_layer_scene(struct cg_output *output, enum zwlr_layer_shell_v1_layer type)
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

	wlr_log(WLR_ERROR, "Invalid layer shell layer: %d", type);
	return NULL;
}

static void popup_handle_destroy(struct wl_listener *listener, void *data);
static void popup_handle_new_popup(struct wl_listener *listener, void *data);
static void popup_handle_commit(struct wl_listener *listener, void *data);

static void
popup_unconstrain(struct cg_layer_popup *popup)
{
	struct wlr_xdg_popup *wlr_popup = popup->wlr_popup;
	struct cg_output *output = popup->toplevel->output;

	if (!output) {
		return;
	}

	int lx, ly;
	wlr_scene_node_coords(&popup->toplevel->scene->tree->node, &lx, &ly);

	struct wlr_box output_box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &output_box);

	struct wlr_box output_toplevel_sx_box = {
		.x = output_box.x - lx,
		.y = output_box.y - ly,
		.width = output->wlr_output->width,
		.height = output->wlr_output->height,
	};

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static void
popup_handle_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->wlr_popup->base->initial_commit) {
		popup_unconstrain(popup);
	}
}

static struct cg_layer_popup *
create_popup(struct wlr_xdg_popup *wlr_popup, struct cg_layer_surface *toplevel, struct wlr_scene_tree *parent)
{
	struct cg_layer_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		return NULL;
	}

	popup->toplevel = toplevel;
	popup->wlr_popup = wlr_popup;
	popup->scene = wlr_scene_xdg_surface_create(parent, wlr_popup->base);

	if (!popup->scene) {
		free(popup);
		return NULL;
	}

	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);

	return popup;
}

static void
popup_handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	wl_list_remove(&popup->commit.link);
	free(popup);
}

static void
popup_handle_new_popup(struct wl_listener *listener, void *data)
{
	struct cg_layer_popup *popup = wl_container_of(listener, popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, popup->toplevel, popup->scene);
}

static void
handle_new_popup(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer_surface = wl_container_of(listener, layer_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	create_popup(wlr_popup, layer_surface, layer_surface->popups);
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *surface = wl_container_of(listener, surface, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = surface->layer_surface;
	uint32_t committed = layer_surface->current.committed;

	if (!surface->output) {
		return;
	}

	if (layer_surface->initial_commit) {
		wlr_fractional_scale_v1_notify_scale(layer_surface->surface, layer_surface->output->scale);
		wlr_surface_set_preferred_buffer_scale(layer_surface->surface, ceil(layer_surface->output->scale));
	}

	if (layer_surface->initialized && committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		enum zwlr_layer_shell_v1_layer layer_type = layer_surface->current.layer;
		struct wlr_scene_tree *output_layer = get_layer_scene(surface->output, layer_type);
		if (output_layer) {
			wlr_scene_node_reparent(&surface->scene->tree->node, output_layer);
			wlr_scene_node_reparent(&surface->popups->node, &surface->output->server->scene->tree);
		}
	}

	bool mapped_changed = layer_surface->surface->mapped != surface->mapped;
	if (!layer_surface->initial_commit && !committed && !mapped_changed) {
		return;
	}

	surface->mapped = layer_surface->surface->mapped;
	arrange_layers(surface->output);
}

static void
handle_map(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *surface = wl_container_of(listener, surface, map);
	struct wlr_layer_surface_v1 *layer_surface = surface->scene->layer_surface;

	if (!surface->output) {
		return;
	}

	struct cg_seat *seat = surface->output->server->seat;

	wlr_scene_node_set_enabled(&surface->scene->tree->node, true);

	if (layer_surface->current.keyboard_interactive &&
	    (layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY ||
	     layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {
		if (!seat->focused_layer || seat->focused_layer->current.layer >= layer_surface->current.layer) {
			seat_set_focus_layer(seat, layer_surface);
		}
	}

	arrange_layers(surface->output);
}

static void
handle_unmap(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *surface = wl_container_of(listener, surface, unmap);

	wlr_scene_node_set_enabled(&surface->scene->tree->node, false);

	if (!surface->output) {
		return;
	}

	struct cg_seat *seat = surface->output->server->seat;

	if (seat->focused_layer == surface->layer_surface) {
		seat_set_focus_layer(seat, NULL);
	}

	arrange_layers(surface->output);
}

static void
handle_node_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, node_destroy);

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

	wl_list_remove(&layer->link);
	free(layer);
}

static struct cg_layer_surface *
create_layer_surface(struct wlr_scene_layer_surface_v1 *scene, struct cg_output *output)
{
	struct cg_layer_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wlr_log(WLR_ERROR, "Could not allocate a layer surface");
		return NULL;
	}

	struct wlr_scene_tree *popups = wlr_scene_tree_create(&output->server->scene->tree);
	if (!popups) {
		wlr_log(WLR_ERROR, "Could not allocate a layer popup node");
		free(surface);
		return NULL;
	}

	surface->scene = scene;
	surface->layer_surface = scene->layer_surface;
	surface->popups = popups;
	surface->layer_surface->data = surface;
	surface->output = output;

	return surface;
}

void
handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_layer_shell_v1_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	wlr_log(WLR_DEBUG, "New layer shell surface: namespace %s layer %d anchor %d size %dx%d margin %d,%d,%d,%d",
		layer_surface->namespace, layer_surface->pending.layer, layer_surface->pending.anchor,
		layer_surface->pending.desired_width, layer_surface->pending.desired_height,
		layer_surface->pending.margin.top, layer_surface->pending.margin.right,
		layer_surface->pending.margin.bottom, layer_surface->pending.margin.left);

	if (!layer_surface->output) {
		if (!wl_list_empty(&server->outputs)) {
			struct cg_output *output = wl_container_of(server->outputs.next, output, link);
			layer_surface->output = output->wlr_output;
		} else {
			wlr_log(WLR_ERROR, "No output to assign layer surface '%s' to", layer_surface->namespace);
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
	}

	struct cg_output *output = layer_surface->output->data;
	if (!output) {
		wlr_log(WLR_ERROR, "Layer surface has no output data");
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	enum zwlr_layer_shell_v1_layer layer_type = layer_surface->pending.layer;
	struct wlr_scene_tree *output_layer = get_layer_scene(output, layer_type);
	if (!output_layer) {
		wlr_log(WLR_ERROR, "Invalid layer %d for layer surface '%s'", layer_type, layer_surface->namespace);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct wlr_scene_layer_surface_v1 *scene_surface =
		wlr_scene_layer_surface_v1_create(output_layer, layer_surface);
	if (!scene_surface) {
		wlr_log(WLR_ERROR, "Could not allocate a layer_surface_v1");
		return;
	}

	struct cg_layer_surface *surface = create_layer_surface(scene_surface, output);
	if (!surface) {
		wlr_layer_surface_v1_destroy(layer_surface);
		wlr_log(WLR_ERROR, "Could not allocate a layer surface");
		return;
	}

	wl_list_insert(&output->layer_surfaces, &surface->link);

	surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &surface->surface_commit);
	surface->map.notify = handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &surface->map);
	surface->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &surface->unmap);
	surface->new_popup.notify = handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &surface->new_popup);

	surface->node_destroy.notify = handle_node_destroy;
	wl_signal_add(&scene_surface->tree->node.events.destroy, &surface->node_destroy);
}
