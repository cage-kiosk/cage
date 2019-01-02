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
#include <wlr/types/wlr_xdg_shell.h>

#include "server.h"
#include "view.h"

static void
activate(struct cg_view *view, bool activate)
{
	wlr_xdg_toplevel_set_activated(view->xdg_surface, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	wlr_xdg_toplevel_set_size(view->xdg_surface, output_width, output_height);
	wlr_xdg_toplevel_set_maximized(view->xdg_surface, true);
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct wlr_box geom;

	wlr_xdg_surface_get_geometry(view->xdg_surface, &geom);
	*width_out = geom.width;
	*height_out = geom.height;
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator,
		 void *data)
{
	wlr_xdg_surface_for_each_surface(view->xdg_surface, iterator, data);
}

static bool
is_primary(struct cg_view *view)
{
	struct wlr_xdg_surface *parent = view->xdg_surface->toplevel->parent;
	/* FIXME: role is 0? */
	return parent == NULL; /*&& role == WLR_XDG_SURFACE_ROLE_TOPLEVEL */
}

static void
handle_xdg_shell_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, unmap);
	view_unmap(view);
}

static void
handle_xdg_shell_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, map);
	view_map(view, view->xdg_surface->surface);
}

static void
handle_xdg_shell_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, destroy);
	view_destroy(view);
}

void
handle_xdg_shell_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	struct cg_view *view = cg_view_create(server);
	view->type = CAGE_XDG_SHELL_VIEW;
	view->xdg_surface = xdg_surface;

	view->map.notify = handle_xdg_shell_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = handle_xdg_shell_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = handle_xdg_shell_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	view->activate = activate;
	view->maximize = maximize;
	view->get_geometry = get_geometry;
	view->for_each_surface = for_each_surface;
	view->is_primary = is_primary;
}
