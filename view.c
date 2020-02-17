/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

static void
view_child_handle_commit(struct wl_listener *listener, void *data)
{
	struct cg_view_child *child = wl_container_of(listener, child, commit);
	view_damage_part(child->view);
}

static void subsurface_create(struct cg_view *view, struct wlr_subsurface *wlr_subsurface);

static void
view_child_handle_new_subsurface(struct wl_listener *listener, void *data)
{
	struct cg_view_child *child = wl_container_of(listener, child, new_subsurface);
	struct wlr_subsurface *wlr_subsurface = data;
	subsurface_create(child->view, wlr_subsurface);
}

void
view_child_finish(struct cg_view_child *child)
{
	if (!child) {
		return;
	}

	view_damage_whole(child->view);

	wl_list_remove(&child->link);
	wl_list_remove(&child->commit.link);
	wl_list_remove(&child->new_subsurface.link);
}

void
view_child_init(struct cg_view_child *child, struct cg_view *view, struct wlr_surface *wlr_surface)
{
	child->view = view;
	child->wlr_surface = wlr_surface;

	child->commit.notify = view_child_handle_commit;
	wl_signal_add(&wlr_surface->events.commit, &child->commit);
	child->new_subsurface.notify = view_child_handle_new_subsurface;
	wl_signal_add(&wlr_surface->events.new_subsurface, &child->new_subsurface);

	wl_list_insert(&view->children, &child->link);
}

static void
subsurface_destroy(struct cg_view_child *child)
{
	if (!child) {
		return;
	}

	struct cg_subsurface *subsurface = (struct cg_subsurface *) child;
	wl_list_remove(&subsurface->destroy.link);
	view_child_finish(&subsurface->view_child);
	free(subsurface);
}

static void
subsurface_handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_subsurface *subsurface = wl_container_of(listener, subsurface, destroy);
	struct cg_view_child *view_child = (struct cg_view_child *) subsurface;
	subsurface_destroy(view_child);
}

static void
subsurface_create(struct cg_view *view, struct wlr_subsurface *wlr_subsurface)
{
	struct cg_subsurface *subsurface = calloc(1, sizeof(struct cg_subsurface));
	if (!subsurface) {
		return;
	}

	view_child_init(&subsurface->view_child, view, wlr_subsurface->surface);
	subsurface->view_child.destroy = subsurface_destroy;
	subsurface->wlr_subsurface = wlr_subsurface;

	subsurface->destroy.notify = subsurface_handle_destroy;
	wl_signal_add(&wlr_subsurface->events.destroy, &subsurface->destroy);
}

static void
handle_new_subsurface(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, new_subsurface);
	struct wlr_subsurface *wlr_subsurface = data;
	subsurface_create(view, wlr_subsurface);
}

char *
view_get_title(struct cg_view *view)
{
	const char *title = view->impl->get_title(view);
	if (!title) {
		return NULL;
	}
	return strndup(title, strlen(title));
}

bool
view_is_primary(struct cg_view *view)
{
	return view->impl->is_primary(view);
}

bool
view_is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	return child->impl->is_transient_for(child, parent);
}

void
view_damage_part(struct cg_view *view)
{
	struct cg_output *output;
	wl_list_for_each (output, &view->server->outputs, link) {
		output_damage_surface(output, view->wlr_surface, view->lx, view->ly, false);
	}
}

void
view_damage_whole(struct cg_view *view)
{
	struct cg_output *output;
	wl_list_for_each (output, &view->server->outputs, link) {
		output_damage_surface(output, view->wlr_surface, view->lx, view->ly, true);
	}
}

void
view_activate(struct cg_view *view, bool activate)
{
	view->impl->activate(view, activate);
}

static bool
view_extends_output_layout(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	return (layout_box->height < height || layout_box->width < width);
}

static void
view_maximize(struct cg_view *view, struct wlr_box *layout_box)
{
	view->lx = layout_box->x;
	view->ly = layout_box->y;
	view->impl->maximize(view, layout_box->width, layout_box->height);
}

static void
view_center(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	view->lx = (layout_box->width - width) / 2;
	view->ly = (layout_box->height - height) / 2;
}

void
view_position(struct cg_view *view)
{
	struct wlr_box *layout_box = wlr_output_layout_get_box(view->server->output_layout, NULL);

	if (view_is_primary(view) || view_extends_output_layout(view, layout_box)) {
		view_maximize(view, layout_box);
	} else {
		view_center(view, layout_box);
	}
}

void
view_for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	view->impl->for_each_surface(view, iterator, data);
}

void
view_for_each_popup(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	if (!view->impl->for_each_popup) {
		return;
	}
	view->impl->for_each_popup(view, iterator, data);
}

void
view_unmap(struct cg_view *view)
{
	wl_list_remove(&view->link);

	wl_list_remove(&view->new_subsurface.link);

	struct cg_view_child *child, *tmp;
	wl_list_for_each_safe (child, tmp, &view->children, link) {
		child->destroy(child);
	}

	view->wlr_surface = NULL;
}

void
view_map(struct cg_view *view, struct wlr_surface *surface)
{
	view->wlr_surface = surface;

	struct wlr_subsurface *subsurface;
	wl_list_for_each (subsurface, &view->wlr_surface->subsurfaces, parent_link) {
		subsurface_create(view, subsurface);
	}

	view->new_subsurface.notify = handle_new_subsurface;
	wl_signal_add(&view->wlr_surface->events.new_subsurface, &view->new_subsurface);

#if CAGE_HAS_XWAYLAND
	/* We shouldn't position override-redirect windows. They set
	   their own (x,y) coordinates in handle_wayland_surface_map. */
	if (view->type != CAGE_XWAYLAND_VIEW || xwayland_view_should_manage(view))
#endif
	{
		view_position(view);
	}

	wl_list_insert(&view->server->views, &view->link);
	seat_set_focus(view->server->seat, view);
}

void
view_destroy(struct cg_view *view)
{
	struct cg_server *server = view->server;
	bool ever_been_mapped = true;

#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW) {
		struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
		ever_been_mapped = xwayland_view->ever_been_mapped;
	}
#endif

	if (view->wlr_surface != NULL) {
		view_unmap(view);
	}

	view->impl->destroy(view);

	/* If there is a previous view in the list, focus that. */
	bool empty = wl_list_empty(&server->views);
	if (!empty) {
		struct cg_view *prev = wl_container_of(server->views.next, prev, link);
		seat_set_focus(server->seat, prev);
	} else if (ever_been_mapped) {
		/* The list is empty and the last view has been
		   mapped, so we can safely exit. */
		wl_display_terminate(server->wl_display);
	}
}

void
view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type, const struct cg_view_impl *impl)
{
	view->server = server;
	view->type = type;
	view->impl = impl;

	wl_list_init(&view->children);
}

struct cg_view *
view_from_wlr_surface(struct cg_server *server, struct wlr_surface *surface)
{
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
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
