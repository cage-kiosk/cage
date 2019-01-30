/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"

char *
view_get_title(struct cg_view *view)
{
	const char *title = view->impl->get_title(view);
	return strndup(title, strlen(title));
}

bool
view_is_primary(struct cg_view *view)
{
	return view->impl->is_primary(view);
}

bool
view_has_children(struct cg_server *server, struct cg_view *parent)
{
	struct cg_view *child;
	wl_list_for_each(child, &server->views, link) {
		if (parent != child && parent->impl->is_parent(parent, child)) {
			return true;
		}
	}
	return false;
}

void
view_activate(struct cg_view *view, bool activate)
{
	view->impl->activate(view, activate);
}

static void
view_maximize(struct cg_view *view)
{
	struct cg_output *output = view->server->output;
	int output_width, output_height;

	wlr_output_transformed_resolution(output->wlr_output, &output_width, &output_height);
	view->impl->maximize(view, output_width, output_height);
}

static void
view_center(struct cg_view *view)
{
	struct wlr_output *output = view->server->output->wlr_output;

	int output_width, output_height;
	wlr_output_transformed_resolution(output, &output_width, &output_height);

	int width, height;
	view->impl->get_geometry(view, &width, &height);

	view->x = (output_width - width) / 2;
	view->y = (output_height - height) / 2;
}

void
view_position(struct cg_view *view)
{
	if (view_is_primary(view)) {
		view_maximize(view);
	} else {
		view_center(view);
	}
}

void
view_for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	view->impl->for_each_surface(view, iterator, data);
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

	view_position(view);

	wl_list_insert(&view->server->views, &view->link);
	seat_set_focus(view->server->seat, view);
}

void
view_destroy(struct cg_view *view)
{
	struct cg_server *server = view->server;

	if (view->wlr_surface != NULL) {
		view_unmap(view);
	}

	view->impl->destroy(view);

	/* If there is a previous view in the list, focus that. */
	bool empty = wl_list_empty(&server->views);
	if (!empty) {
		struct cg_view *prev = wl_container_of(server->views.next, prev, link);
		seat_set_focus(server->seat, prev);
	} else {
		/* The list is empty and the last view has been
		   mapped, so we can safely exit. */
		wl_display_terminate(server->wl_display);
	}
}

void
view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type,
	  const struct cg_view_impl *impl)
{
	view->server = server;
	view->type = type;
	view->impl = impl;
}

struct cg_view *
view_from_wlr_surface(struct cg_server *server, struct wlr_surface *surface)
{
	struct cg_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->wlr_surface == surface) {
			return view;
		}
	}
	return NULL;
}

struct wlr_surface *
view_wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y)
{
	return view->impl->wlr_surface_at(view, sx, sy, sub_x, sub_y);
}
