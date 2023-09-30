/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"
#include "view.h"
#include "xdg_shell.h"

static void
xdg_decoration_handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xdg_decoration *xdg_decoration = wl_container_of(listener, xdg_decoration, destroy);

	wl_list_remove(&xdg_decoration->destroy.link);
	wl_list_remove(&xdg_decoration->request_mode.link);
	free(xdg_decoration);
}

static void
xdg_decoration_handle_request_mode(struct wl_listener *listener, void *data)
{
	struct cg_xdg_decoration *xdg_decoration = wl_container_of(listener, xdg_decoration, request_mode);
	enum wlr_xdg_toplevel_decoration_v1_mode mode;

	if (xdg_decoration->server->xdg_decoration) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	} else {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(xdg_decoration->wlr_decoration, mode);
}

static struct cg_view *
popup_get_view(struct wlr_xdg_popup *popup)
{
	while (true) {
		if (popup->parent == NULL) {
			return NULL;
		}
		struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
		if (xdg_surface == NULL) {
			return NULL;
		}
		switch (xdg_surface->role) {
		case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
			return xdg_surface->data;
		case WLR_XDG_SURFACE_ROLE_POPUP:
			popup = xdg_surface->popup;
			break;
		case WLR_XDG_SURFACE_ROLE_NONE:
			return NULL;
		}
	}
}

static void
popup_unconstrain(struct cg_view *view, struct wlr_xdg_popup *popup)
{
	struct cg_server *server = view->server;
	struct wlr_box *popup_box = &popup->current.geometry;

	struct wlr_output_layout *output_layout = server->output_layout;
	struct wlr_output *wlr_output =
		wlr_output_layout_output_at(output_layout, view->lx + popup_box->x, view->ly + popup_box->y);
	struct wlr_box output_box;
	wlr_output_layout_get_box(output_layout, wlr_output, &output_box);

	struct wlr_box output_toplevel_box = {
		.x = output_box.x - view->lx,
		.y = output_box.y - view->ly,
		.width = output_box.width,
		.height = output_box.height,
	};

	wlr_xdg_popup_unconstrain_from_box(popup, &output_toplevel_box);
}

static struct cg_xdg_shell_view *
xdg_shell_view_from_view(struct cg_view *view)
{
	return (struct cg_xdg_shell_view *) view;
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return xdg_shell_view->xdg_toplevel->title;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_box geom;

	wlr_xdg_surface_get_geometry(xdg_shell_view->xdg_toplevel->base, &geom);
	*width_out = geom.width;
	*height_out = geom.height;
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_xdg_toplevel *parent = xdg_shell_view->xdg_toplevel->parent;

	return parent == NULL;
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	if (parent->type != CAGE_XDG_SHELL_VIEW) {
		return false;
	}
	struct cg_xdg_shell_view *_child = xdg_shell_view_from_view(child);
	struct wlr_xdg_toplevel *xdg_toplevel = _child->xdg_toplevel;
	struct cg_xdg_shell_view *_parent = xdg_shell_view_from_view(parent);
	while (xdg_toplevel) {
		if (xdg_toplevel->parent == _parent->xdg_toplevel) {
			return true;
		}
		xdg_toplevel = xdg_toplevel->parent;
	}
	return false;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_activated(xdg_shell_view->xdg_toplevel, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_size(xdg_shell_view->xdg_toplevel, output_width, output_height);
	wlr_xdg_toplevel_set_maximized(xdg_shell_view->xdg_toplevel, true);
}

static void
destroy(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	free(xdg_shell_view);
}

static void
handle_xdg_shell_surface_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, request_fullscreen);

	/**
	 * Certain clients do not like figuring out their own window geometry if they
	 * display in fullscreen mode, so we set it here.
	 */
	struct wlr_box layout_box;
	wlr_output_layout_get_box(xdg_shell_view->view.server->output_layout, NULL, &layout_box);
	wlr_xdg_toplevel_set_size(xdg_shell_view->xdg_toplevel, layout_box.width, layout_box.height);

	wlr_xdg_toplevel_set_fullscreen(xdg_shell_view->xdg_toplevel,
					xdg_shell_view->xdg_toplevel->requested.fullscreen);
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

	view_map(view, xdg_shell_view->xdg_toplevel->base->surface);
}

static void
handle_xdg_shell_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, destroy);
	struct cg_view *view = &xdg_shell_view->view;

	wl_list_remove(&xdg_shell_view->map.link);
	wl_list_remove(&xdg_shell_view->unmap.link);
	wl_list_remove(&xdg_shell_view->destroy.link);
	wl_list_remove(&xdg_shell_view->request_fullscreen.link);
	xdg_shell_view->xdg_toplevel = NULL;

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
};

void
handle_xdg_shell_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	switch (xdg_surface->role) {
	case WLR_XDG_SURFACE_ROLE_TOPLEVEL:;
		struct cg_xdg_shell_view *xdg_shell_view = calloc(1, sizeof(struct cg_xdg_shell_view));
		if (!xdg_shell_view) {
			wlr_log(WLR_ERROR, "Failed to allocate XDG Shell view");
			return;
		}

		view_init(&xdg_shell_view->view, server, CAGE_XDG_SHELL_VIEW, &xdg_shell_view_impl);
		xdg_shell_view->xdg_toplevel = xdg_surface->toplevel;

		xdg_shell_view->map.notify = handle_xdg_shell_surface_map;
		wl_signal_add(&xdg_surface->events.map, &xdg_shell_view->map);
		xdg_shell_view->unmap.notify = handle_xdg_shell_surface_unmap;
		wl_signal_add(&xdg_surface->events.unmap, &xdg_shell_view->unmap);
		xdg_shell_view->destroy.notify = handle_xdg_shell_surface_destroy;
		wl_signal_add(&xdg_surface->events.destroy, &xdg_shell_view->destroy);
		xdg_shell_view->request_fullscreen.notify = handle_xdg_shell_surface_request_fullscreen;
		wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &xdg_shell_view->request_fullscreen);

		xdg_surface->data = xdg_shell_view;
		break;
	case WLR_XDG_SURFACE_ROLE_POPUP:;
		struct wlr_xdg_popup *popup = xdg_surface->popup;
		struct cg_view *view = popup_get_view(popup);
		if (view == NULL) {
			return;
		}

		struct wlr_scene_tree *parent_scene_tree = NULL;
		struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
		if (parent == NULL) {
			return;
		}
		switch (parent->role) {
		case WLR_XDG_SURFACE_ROLE_TOPLEVEL:;
			parent_scene_tree = view->scene_tree;
			break;
		case WLR_XDG_SURFACE_ROLE_POPUP:
			parent_scene_tree = parent->data;
			break;
		case WLR_XDG_SURFACE_ROLE_NONE:
			break;
		}
		if (parent_scene_tree == NULL) {
			return;
		}

		struct wlr_scene_tree *popup_scene_tree = wlr_scene_xdg_surface_create(parent_scene_tree, xdg_surface);
		if (popup_scene_tree == NULL) {
			wlr_log(WLR_ERROR, "Failed to allocate scene-graph node for XDG popup");
			return;
		}

		popup_unconstrain(view, popup);

		xdg_surface->data = popup_scene_tree;
		break;
	case WLR_XDG_SURFACE_ROLE_NONE:
		assert(false); // unreachable
	}
}

void
handle_xdg_toplevel_decoration(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, xdg_toplevel_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;

	struct cg_xdg_decoration *xdg_decoration = calloc(1, sizeof(struct cg_xdg_decoration));
	if (!xdg_decoration) {
		return;
	}

	xdg_decoration->wlr_decoration = wlr_decoration;
	xdg_decoration->server = server;

	xdg_decoration->destroy.notify = xdg_decoration_handle_destroy;
	wl_signal_add(&wlr_decoration->events.destroy, &xdg_decoration->destroy);
	xdg_decoration->request_mode.notify = xdg_decoration_handle_request_mode;
	wl_signal_add(&wlr_decoration->events.request_mode, &xdg_decoration->request_mode);

	xdg_decoration_handle_request_mode(&xdg_decoration->request_mode, wlr_decoration);
}
