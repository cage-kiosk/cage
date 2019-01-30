/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"
#include "view.h"
#include "xdg_shell.h"

static struct cg_xdg_shell_view *
xdg_shell_view_from_view(struct cg_view *view)
{
	return (struct cg_xdg_shell_view *) view;
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return xdg_shell_view->xdg_surface->toplevel->title;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_box geom;

	wlr_xdg_surface_get_geometry(xdg_shell_view->xdg_surface, &geom);
	*width_out = geom.width;
	*height_out = geom.height;
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_xdg_surface *parent = xdg_shell_view->xdg_surface->toplevel->parent;
	/* FIXME: role is 0? */
	return parent == NULL; /*&& role == WLR_XDG_SURFACE_ROLE_TOPLEVEL */
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	if (parent->type != CAGE_XDG_SHELL_VIEW) {
		return false;
	}
	struct cg_xdg_shell_view *_child = xdg_shell_view_from_view(child);
	struct wlr_xdg_surface *xdg_surface = _child->xdg_surface;
	struct cg_xdg_shell_view *_parent = xdg_shell_view_from_view(parent);
	struct wlr_xdg_surface *parent_xdg_surface = _parent->xdg_surface;
	while (xdg_surface) {
		if (xdg_surface->toplevel->parent == parent_xdg_surface) {
			return true;
		}
		xdg_surface = xdg_surface->toplevel->parent;
	}
	return false;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_activated(xdg_shell_view->xdg_surface, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_size(xdg_shell_view->xdg_surface, output_width, output_height);
	wlr_xdg_toplevel_set_maximized(xdg_shell_view->xdg_surface, true);
}

static void
destroy(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	free(xdg_shell_view);
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_surface_for_each_surface(xdg_shell_view->xdg_surface, iterator, data);
}

static struct wlr_surface *
wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return wlr_xdg_surface_surface_at(xdg_shell_view->xdg_surface, sx, sy, sub_x, sub_y);
}

static void
handle_xdg_shell_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, unmap);
	struct cg_view *view = &xdg_shell_view->view;

	view_unmap(view);
}

static void
handle_xdg_shell_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, map);
	struct cg_view *view = &xdg_shell_view->view;

	view_map(view, xdg_shell_view->xdg_surface->surface);
}

static void
handle_xdg_shell_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, destroy);
	struct cg_view *view = &xdg_shell_view->view;

	wl_list_remove(&xdg_shell_view->map.link);
	wl_list_remove(&xdg_shell_view->unmap.link);
	wl_list_remove(&xdg_shell_view->destroy.link);
	xdg_shell_view->xdg_surface = NULL;

	view_destroy(view);
}

static const struct cg_view_impl xdg_shell_view_impl = {
	.get_title = get_title,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = activate,
	.maximize = maximize,
	.destroy = destroy,
	.for_each_surface = for_each_surface,
	.wlr_surface_at = wlr_surface_at,
};

void
handle_xdg_shell_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	struct cg_xdg_shell_view *xdg_shell_view = calloc(1, sizeof(struct cg_xdg_shell_view));
	if (!xdg_shell_view) {
		wlr_log(WLR_ERROR, "Failed to allocate XDG Shell view");
		return;
	}

	view_init(&xdg_shell_view->view, server, CAGE_XDG_SHELL_VIEW, &xdg_shell_view_impl);
	xdg_shell_view->xdg_surface = xdg_surface;

	xdg_shell_view->map.notify = handle_xdg_shell_surface_map;
	wl_signal_add(&xdg_surface->events.map, &xdg_shell_view->map);
	xdg_shell_view->unmap.notify = handle_xdg_shell_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &xdg_shell_view->unmap);
	xdg_shell_view->destroy.notify = handle_xdg_shell_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &xdg_shell_view->destroy);
}
