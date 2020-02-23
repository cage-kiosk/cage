/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "layer_shell_v1.h"
#include "server.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

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
}
