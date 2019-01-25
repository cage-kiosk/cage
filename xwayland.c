/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/xwayland.h>

#include "server.h"
#include "view.h"
#include "xwayland.h"

static char *
get_title(struct cg_view *view)
{
	return view->xwayland_surface->title;
}

static void
activate(struct cg_view *view, bool activate)
{
	wlr_xwayland_surface_activate(view->xwayland_surface, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	wlr_xwayland_surface_configure(view->xwayland_surface, 0, 0, output_width, output_height);
	wlr_xwayland_surface_set_maximized(view->xwayland_surface, true);
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	*width_out = view->xwayland_surface->surface->current.width;
	*height_out = view->xwayland_surface->surface->current.height;
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator,
		 void *data)
{
	wlr_surface_for_each_surface(view->wlr_surface, iterator, data);
}

static struct wlr_surface *
wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y)
{
	return wlr_surface_surface_at(view->wlr_surface, sx, sy, sub_x, sub_y);
}

static bool
is_primary(struct cg_view *view)
{
	struct wlr_xwayland_surface *parent = view->xwayland_surface->parent;
	return parent == NULL;
}

static bool
is_parent(struct cg_view *parent, struct cg_view *child)
{
	if (child->type != CAGE_XWAYLAND_VIEW) {
		return false;
	}
	return child->xwayland_surface->parent == parent->xwayland_surface;
}

static void
handle_xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, unmap);
	view_unmap(view);
}

static void
handle_xwayland_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, map);
	view_map(view, view->xwayland_surface->surface);
}

static void
handle_xwayland_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, destroy);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);

	view_destroy(view);
}

void
handle_xwayland_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xwayland_surface = data;

	struct cg_view *view = cg_view_create(server);
	view->type = CAGE_XWAYLAND_VIEW;
	view->xwayland_surface = xwayland_surface;

	view->map.notify = handle_xwayland_surface_map;
	wl_signal_add(&xwayland_surface->events.map, &view->map);
	view->unmap.notify = handle_xwayland_surface_unmap;
	wl_signal_add(&xwayland_surface->events.unmap, &view->unmap);
	view->destroy.notify = handle_xwayland_surface_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &view->destroy);

	view->get_title = get_title;
	view->activate = activate;
	view->maximize = maximize;
	view->get_geometry = get_geometry;
	view->for_each_surface = for_each_surface;
	view->wlr_surface_at = wlr_surface_at;
	view->is_primary = is_primary;
	view->is_parent = is_parent;
}
