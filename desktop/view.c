/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <wlr/types/wlr_surface.h>

#include "output.h"
#include "view.h"

static bool
cage_view_extends_output(struct cg_view *view, struct wlr_box *output_box)
{
	assert(view->impl->get_geometry != NULL);

	int width, height;
	view->impl->get_geometry(view, &width, &height);

	return (output_box->height < height || output_box->width < width);
}

bool
cage_view_is_primary(struct cg_view *view)
{
	assert(view != NULL);
	assert(view->impl->is_primary != NULL);
	return view->impl->is_primary(view);
}

static void
cage_view_maximize(struct cg_view *view, struct wlr_box *output_box)
{
	assert(view->impl->maximize != NULL);

	view->impl->maximize(view, output_box->width, output_box->height);
}

static void
cage_view_center(struct cg_view *view, struct wlr_box *output_box)
{
	// No-op placeholder
}

void
cage_view_position(struct cg_view *view)
{
	assert(view != NULL);

	struct wlr_box output_box = {0};
	cage_output_get_geometry(view->output, &output_box);

	if (cage_view_is_primary(view) || cage_view_extends_output(view, &output_box)) {
		cage_view_maximize(view, &output_box);
	} else {
		cage_view_center(view, &output_box);
	}
}

void
cage_view_for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *user_data)
{
	assert(view != NULL);
	assert(view->impl->for_each_surface != NULL);
	view->impl->for_each_surface(view, iterator, user_data);
}

char *
cage_view_get_title(struct cg_view *view)
{
	assert(view != NULL);
	assert(view->impl->get_title != NULL);

	const char *title = view->impl->get_title(view);
	if (!title) {
		return NULL;
	}
	return strndup(title, strlen(title));
}

void
cage_view_activate(struct cg_view *view, bool activate)
{
	assert(view != NULL);
	assert(view->impl->activate != NULL);
	view->impl->activate(view, activate);
}

bool
cage_view_is_mapped(struct cg_view *view)
{
	assert(view != NULL);
	return view->wlr_surface != NULL;
}

void
cage_view_unmap(struct cg_view *view)
{
	assert(view != NULL);
	assert(cage_view_is_mapped(view));

	wl_signal_emit(&view->events.unmap, view);

	wl_list_remove(&view->link);
	wl_list_init(&view->link);

	view->wlr_surface = NULL;

	assert(!cage_view_is_mapped(view));
}

void
cage_view_map(struct cg_view *view, struct wlr_surface *surface)
{
	assert(view != NULL);
	assert(surface != NULL);
	assert(!cage_view_is_mapped(view));

	view->wlr_surface = surface;

	wl_list_insert(&view->output->views, &view->link);
	cage_view_position(view);

	wl_signal_emit(&view->events.map, view);

	assert(cage_view_is_mapped(view));
}

void
cage_view_fini(struct cg_view *view)
{
	assert(view != NULL);
	assert(!cage_view_is_mapped(view));
	view->output = NULL;
}

void
cage_view_init(struct cg_view *view, enum cg_view_type type, const struct cg_view_impl *impl, struct cg_output *output)
{
	assert(view != NULL);
	assert(impl != NULL);
	assert(output != NULL);

	view->type = type;
	view->impl = impl;
	view->output = output;

	wl_signal_init(&view->events.map);
	wl_signal_init(&view->events.unmap);
}
