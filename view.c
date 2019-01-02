/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"

void
view_activate(struct cg_view *view, bool activate)
{
	view->activate(view, activate);
}

void
view_maximize(struct cg_view *view)
{
	struct cg_output *output = view->server->output;
	int output_width, output_height;

	wlr_output_effective_resolution(output->wlr_output, &output_width, &output_height);
	view->maximize(view, output_width, output_height);
}

void
view_center(struct cg_view *view)
{
	struct cg_server *server = view->server;
	struct wlr_output *output = server->output->wlr_output;

	int output_width, output_height;
	wlr_output_effective_resolution(output, &output_width, &output_height);

	int width, height;
	view->get_geometry(view, &width, &height);

	view->x = (output_width - width) / 2;
	view->y = (output_height - height) / 2;
}

bool
view_is_primary(struct cg_view *view)
{
	return view->is_primary(view);
}

void
view_unmap(struct cg_view *view)
{
	wl_list_remove(&view->link);
	view->wlr_surface = NULL;
}

void
view_map(struct cg_view *view, struct wlr_surface *surface)
{
	view->wlr_surface = surface;

	if (view_is_primary(view)) {
		view_maximize(view);
	} else {
		view_center(view);	
	}

	wl_list_insert(&view->server->views, &view->link);
	seat_set_focus(view->server->seat, view);
}

void
view_destroy(struct cg_view *view)
{
	struct cg_server *server = view->server;
	bool terminate = view_is_primary(view);

	free(view);

	/* If this was our primary view, exit. */
	if (terminate) {
		wl_display_terminate(server->wl_display);
	}
}

struct cg_view *
cg_view_create(struct cg_server *server)
{
	struct cg_view *view = calloc(1, sizeof(struct cg_view));

	view->server = server;
	return view;
}

struct cg_view *
cg_view_from_wlr_surface(struct cg_server *server, struct wlr_surface *surface)
{
	struct cg_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->wlr_surface == surface) {
			return view;
		}
	}
	return NULL;
}
