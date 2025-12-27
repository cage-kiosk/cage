/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "server.h"
#include "view.h"
#include "xwayland.h"

struct cg_xwayland_view *
xwayland_view_from_view(struct cg_view *view)
{
	return (struct cg_xwayland_view *) view;
}

bool
xwayland_view_should_manage(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *xwayland_surface = xwayland_view->xwayland_surface;
	return !xwayland_surface->override_redirect;
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	return xwayland_view->xwayland_surface->title;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;
	if (xsurface->surface == NULL) {
		*width_out = 0;
		*height_out = 0;
		return;
	}

	*width_out = xsurface->surface->current.width;
	*height_out = xsurface->surface->current.height;
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *parent = xwayland_view->xwayland_surface->parent;
	return parent == NULL;
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	if (parent->type != CAGE_XDG_SHELL_VIEW) {
		return false;
	}
	struct cg_xwayland_view *_child = xwayland_view_from_view(child);
	struct wlr_xwayland_surface *xwayland_surface = _child->xwayland_surface;
	struct cg_xwayland_view *_parent = xwayland_view_from_view(parent);
	struct wlr_xwayland_surface *parent_xwayland_surface = _parent->xwayland_surface;
	while (xwayland_surface) {
		if (xwayland_surface->parent == parent_xwayland_surface) {
			return true;
		}
		xwayland_surface = xwayland_surface->parent;
	}
	return false;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	wlr_xwayland_surface_activate(xwayland_view->xwayland_surface, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	wlr_xwayland_surface_configure(xwayland_view->xwayland_surface, view->lx, view->ly, output_width,
				       output_height);
	wlr_xwayland_surface_set_maximized(xwayland_view->xwayland_surface, true, true);
}

static void
destroy(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	free(xwayland_view);
}

static void
close(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	wlr_xwayland_surface_close(xwayland_view->xwayland_surface);
}

static void
handle_xwayland_surface_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, request_fullscreen);
	struct wlr_xwayland_surface *xwayland_surface = xwayland_view->xwayland_surface;
	wlr_xwayland_surface_set_fullscreen(xwayland_view->xwayland_surface, xwayland_surface->fullscreen);
	if (xwayland_view->view.foreign_toplevel_handle) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(xwayland_view->view.foreign_toplevel_handle,
							      xwayland_surface->fullscreen);
	}
}

static void
handle_xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, unmap);
	struct cg_view *view = &xwayland_view->view;

	view_unmap(view);
}

static void
handle_xwayland_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, map);
	struct cg_view *view = &xwayland_view->view;

	if (!xwayland_view_should_manage(view)) {
		view->lx = xwayland_view->xwayland_surface->x;
		view->ly = xwayland_view->xwayland_surface->y;
	}

	view_map(view, xwayland_view->xwayland_surface->surface);

	if (xwayland_view->xwayland_surface->title)
		wlr_foreign_toplevel_handle_v1_set_title(view->foreign_toplevel_handle,
							 xwayland_view->xwayland_surface->title);
	if (xwayland_view->xwayland_surface->class)
		wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_toplevel_handle,
							  xwayland_view->xwayland_surface->class);
	wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign_toplevel_handle,
						      xwayland_view->xwayland_surface->fullscreen);
}

static void
handle_xwayland_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, destroy);
	struct cg_view *view = &xwayland_view->view;

	wl_list_remove(&xwayland_view->associate.link);
	wl_list_remove(&xwayland_view->dissociate.link);
	wl_list_remove(&xwayland_view->destroy.link);
	wl_list_remove(&xwayland_view->request_fullscreen.link);
	xwayland_view->xwayland_surface = NULL;

	view_destroy(view);
}

static const struct cg_view_impl xwayland_view_impl = {
	.get_title = get_title,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = activate,
	.maximize = maximize,
	.destroy = destroy,
	.close = close,
};

void
handle_xwayland_associate(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, associate);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	xwayland_view->map.notify = handle_xwayland_surface_map;
	wl_signal_add(&xsurface->surface->events.map, &xwayland_view->map);
	xwayland_view->unmap.notify = handle_xwayland_surface_unmap;
	wl_signal_add(&xsurface->surface->events.unmap, &xwayland_view->unmap);
}

void
handle_xwayland_dissociate(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, dissociate);
	wl_list_remove(&xwayland_view->map.link);
	wl_list_remove(&xwayland_view->unmap.link);
}

void
handle_xwayland_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xwayland_surface = data;

	struct cg_xwayland_view *xwayland_view = calloc(1, sizeof(struct cg_xwayland_view));
	if (!xwayland_view) {
		wlr_log(WLR_ERROR, "Failed to allocate XWayland view");
		return;
	}

	view_init(&xwayland_view->view, server, CAGE_XWAYLAND_VIEW, &xwayland_view_impl);
	xwayland_view->xwayland_surface = xwayland_surface;

	xwayland_view->associate.notify = handle_xwayland_associate;
	wl_signal_add(&xwayland_surface->events.associate, &xwayland_view->associate);
	xwayland_view->dissociate.notify = handle_xwayland_dissociate;
	wl_signal_add(&xwayland_surface->events.dissociate, &xwayland_view->dissociate);
	xwayland_view->destroy.notify = handle_xwayland_surface_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &xwayland_view->destroy);
	xwayland_view->request_fullscreen.notify = handle_xwayland_surface_request_fullscreen;
	wl_signal_add(&xwayland_surface->events.request_fullscreen, &xwayland_view->request_fullscreen);
}
