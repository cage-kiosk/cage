/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "view.h"
#include "xdg_shell.h"

static struct cg_xdg_shell_view *
xdg_shell_view_from_view(struct cg_view *view)
{
	return (struct cg_xdg_shell_view *) view;
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_surface_for_each_surface(xdg_shell_view->xdg_surface, iterator, user_data);
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_box geometry;

	wlr_xdg_surface_get_geometry(xdg_shell_view->xdg_surface, &geometry);
	*width_out = geometry.width;
	*height_out = geometry.height;
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_size(xdg_shell_view->xdg_surface, output_width, output_height);
	wlr_xdg_toplevel_set_maximized(xdg_shell_view->xdg_surface, true);
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_xdg_surface *xdg_surface = xdg_shell_view->xdg_surface;

	struct wlr_xdg_surface *parent = xdg_surface->toplevel->parent;
	enum wlr_xdg_surface_role role = xdg_surface->role;

	return parent == NULL && role == WLR_XDG_SURFACE_ROLE_TOPLEVEL;
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return xdg_shell_view->xdg_surface->toplevel->title;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_activated(xdg_shell_view->xdg_surface, activate);
}

static void
handle_xdg_shell_surface_unmap(struct wl_listener *listener, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, unmap);
	struct cg_view *view = &xdg_shell_view->view;

	assert(cage_view_is_mapped(view));

	cage_view_damage_whole(view);

	wl_list_remove(&xdg_shell_view->commit.link);
	wl_list_remove(&xdg_shell_view->request_fullscreen.link);

	cage_view_unmap(view);

	assert(!cage_view_is_mapped(view));
}

static void
handle_xdg_shell_surface_request_fullscreen(struct wl_listener *listener, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, request_fullscreen);
	struct wlr_xdg_toplevel_set_fullscreen_event *event = user_data;
	wlr_xdg_toplevel_set_fullscreen(xdg_shell_view->xdg_surface, event->fullscreen);
}

static void
handle_xdg_shell_surface_commit(struct wl_listener *listener, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, commit);
	struct cg_view *view = &xdg_shell_view->view;
	cage_view_damage_part(view);
}

static void
handle_xdg_shell_surface_destroy(struct wl_listener *listener, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, destroy);
	struct cg_view *view = &xdg_shell_view->view;

	if (cage_view_is_mapped(view)) {
		handle_xdg_shell_surface_unmap(listener, user_data);
	}

	wl_list_remove(&xdg_shell_view->map.link);
	wl_list_remove(&xdg_shell_view->unmap.link);
	wl_list_remove(&xdg_shell_view->destroy.link);
	xdg_shell_view->xdg_surface = NULL;

	cage_view_fini(view);
	free(xdg_shell_view);
}

static void
handle_xdg_shell_surface_map(struct wl_listener *listener, void *user_data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, map);
	struct cg_view *view = &xdg_shell_view->view;

	assert(!cage_view_is_mapped(view));

	xdg_shell_view->commit.notify = handle_xdg_shell_surface_commit;
	wl_signal_add(&xdg_shell_view->xdg_surface->surface->events.commit, &xdg_shell_view->commit);
	xdg_shell_view->request_fullscreen.notify = handle_xdg_shell_surface_request_fullscreen;
	wl_signal_add(&xdg_shell_view->xdg_surface->toplevel->events.request_fullscreen,
		      &xdg_shell_view->request_fullscreen);

	cage_view_map(view, xdg_shell_view->xdg_surface->surface);

	cage_view_damage_whole(view);
	assert(cage_view_is_mapped(view));
}

static const struct cg_view_impl xdg_shell_view_impl = {
	.for_each_surface = for_each_surface,
	.get_geometry = get_geometry,
	.maximize = maximize,
	.is_primary = is_primary,
	.get_title = get_title,
	.activate = activate,
};

void
cage_xdg_shell_view_init(struct cg_xdg_shell_view *xdg_shell_view, struct wlr_xdg_surface *xdg_surface,
			 struct cg_output *output)
{
	assert(xdg_shell_view != NULL);
	assert(xdg_surface != NULL);
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	cage_view_init(&xdg_shell_view->view, CAGE_XDG_SHELL_VIEW, &xdg_shell_view_impl, output);
	xdg_shell_view->xdg_surface = xdg_surface;
	xdg_shell_view->xdg_surface->data = xdg_shell_view;

	xdg_shell_view->map.notify = handle_xdg_shell_surface_map;
	wl_signal_add(&xdg_surface->events.map, &xdg_shell_view->map);
	xdg_shell_view->unmap.notify = handle_xdg_shell_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &xdg_shell_view->unmap);
	xdg_shell_view->destroy.notify = handle_xdg_shell_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &xdg_shell_view->destroy);
}
