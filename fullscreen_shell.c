/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_fullscreen_shell_v1.h>
#include <wlr/util/log.h>

#include "fullscreen_shell.h"
#include "output.h"
#include "server.h"
#include "view.h"

static struct cg_fullscreen_shell_view *
fullscreen_shell_view_from_view(struct cg_view *view)
{
	return (struct cg_fullscreen_shell_view *) view;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	// struct wlr_box *layout_box = wlr_output_layout_get_box(view->server->output_layout, NULL);
	// *width_out = layout_box->width;
	// *height_out = layout_box->height;
	*width_out = view->wlr_surface->current.width;
	*height_out = view->wlr_surface->current.height;
}

static bool
is_primary(struct cg_view *view)
{
	return true;
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	return false;
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	// view->wlr_surface->pending.width = output_width;
	// view->wlr_surface->pending.height = output_height;
}

static void
destroy(struct cg_view *view)
{
	struct cg_fullscreen_shell_view *fullscreen_shell_view = fullscreen_shell_view_from_view(view);
	free(fullscreen_shell_view);
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	wlr_surface_for_each_surface(view->wlr_surface, iterator, data);
}

static struct wlr_surface *
wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y)
{
	return wlr_surface_surface_at(view->wlr_surface, sx, sy, sub_x, sub_y);
}

//static void output_set_surface(struct cg_output *output, struct wlr_surface *surface);
//
//static void
//output_handle_surface_destroy(struct wl_listener *listener, void *data)
//{
//	struct cg_output *output = wl_container_of(listener, output, surface_destroy);
//	output_set_surface(output, NULL);
//}
//
//static void
//output_set_surface(struct cg_output *output, struct wlr_surface *surface)
//{
//	if (output->surface == surface) {
//		return;
//	}
//
//	if (output->surface) {
//		wl_list_remove(&output->surface_destroy.link);
//		output->surface = NULL;
//	}
//
//	if (surface) {
//		output->surface_destroy.notify = output_handle_surface_destroy;
//		wl_signal_add(&surface->events.destroy, &output->surface_destroy);
//		output->surface = surface;
//	}
//
//	wlr_log(WLR_DEBUG, "Presenting fullscreen shell surface %p on output %s", surface, output->wlr_output->name);
//}

static const struct cg_view_impl fullscreen_shell_view_impl = {
	.get_title = NULL,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = NULL,
	.maximize = maximize,
	.destroy = destroy,
	.for_each_surface = for_each_surface,
	.for_each_popup = NULL,
	.wlr_surface_at = wlr_surface_at,
};

void
handle_fullscreen_shell_present_surface(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, fullscreen_shell_present_surface);
	struct wlr_fullscreen_shell_v1_present_surface_event *event = data;

	struct cg_fullscreen_shell_view *fullscreen_shell_view = calloc(1, sizeof(struct cg_fullscreen_shell_view));
	if (!fullscreen_shell_view) {
		wlr_log(WLR_ERROR, "Failed to allocate Fullscreen Shell view");
		return;
	}

	view_init(&fullscreen_shell_view->view, server, CAGE_FULLSCREEN_SHELL_VIEW, &fullscreen_shell_view_impl);
	view_map(&fullscreen_shell_view->view, event->surface);
}
