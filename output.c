/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "util.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

static void
output_enable(struct cg_output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;

	/* Outputs get enabled by the backend before firing the new_output event,
	 * so we can't do a check for already enabled outputs here unless we
	 * duplicate the enabled property in cg_output. */
	wlr_log(WLR_DEBUG, "Enabling output %s", wlr_output->name);

	wlr_output_layout_add_auto(output->server->output_layout, wlr_output);
	wlr_output_enable(wlr_output, true);
	wlr_output_commit(wlr_output);

	struct wlr_scene_output *scene_output;
	wl_list_for_each (scene_output, &output->server->scene->outputs, link) {
		if (scene_output->output == wlr_output) {
			output->scene_output = scene_output;
			break;
		}
	}
	assert(output->scene_output != NULL);
}

static void
output_disable(struct cg_output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;

	if (!wlr_output->enabled) {
		wlr_log(WLR_DEBUG, "Not disabling already disabled output %s", wlr_output->name);
		return;
	}

	output->scene_output = NULL;

	wlr_log(WLR_DEBUG, "Disabling output %s", wlr_output->name);
	wlr_output_enable(wlr_output, false);
	wlr_output_layout_remove(output->server->output_layout, wlr_output);
	wlr_output_commit(wlr_output);
}

static void
handle_output_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, frame);

	if (!output->wlr_output->enabled) {
		return;
	}

	wlr_scene_output_commit(output->scene_output);

	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void
handle_output_commit(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;

	if (!output->wlr_output->enabled) {
		return;
	}

	if (event->committed & WLR_OUTPUT_STATE_TRANSFORM) {
		struct cg_view *view;
		wl_list_for_each (view, &output->server->views, link) {
			view_position(view);
		}
	}
}

static void
handle_output_mode(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, mode);

	if (!output->wlr_output->enabled) {
		return;
	}

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}
}

static void
output_destroy(struct cg_output *output)
{
	struct cg_server *server = output->server;

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->mode.link);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->link);

	wlr_output_layout_remove(server->output_layout, output->wlr_output);

	free(output);

	if (wl_list_empty(&server->outputs)) {
		wl_display_terminate(server->wl_display);
	} else if (server->output_mode == CAGE_MULTI_OUTPUT_MODE_LAST) {
		struct cg_output *prev = wl_container_of(server->outputs.next, prev, link);
		if (prev) {
			output_enable(prev);

			struct cg_view *view;
			wl_list_for_each (view, &server->views, link) {
				view_position(view);
			}
		}
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, destroy);
	output_destroy(output);
}

void
handle_new_output(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Failed to initialize output rendering");
		return;
	}

	struct cg_output *output = calloc(1, sizeof(struct cg_output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output");
		return;
	}

	output->wlr_output = wlr_output;
	output->server = server;

	wl_list_insert(&server->outputs, &output->link);

	output->commit.notify = handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->mode.notify = handle_output_mode;
	wl_signal_add(&wlr_output->events.mode, &output->mode);
	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	struct wlr_output_mode *preferred_mode = wlr_output_preferred_mode(wlr_output);
	if (preferred_mode) {
		wlr_output_set_mode(wlr_output, preferred_mode);
	}
	wlr_output_set_transform(wlr_output, output->server->output_transform);

	if (server->output_mode == CAGE_MULTI_OUTPUT_MODE_LAST) {
		struct cg_output *next = wl_container_of(output->link.next, next, link);
		if (next) {
			output_disable(next);
		}
	}

	if (!wlr_xcursor_manager_load(server->seat->xcursor_manager, wlr_output->scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for output '%s' with scale %f", wlr_output->name,
			wlr_output->scale);
	}

	output_enable(output);

	struct cg_view *view;
	wl_list_for_each (view, &output->server->views, link) {
		view_position(view);
	}
}

void
output_set_window_title(struct cg_output *output, const char *title)
{
	struct wlr_output *wlr_output = output->wlr_output;

	if (!wlr_output->enabled) {
		wlr_log(WLR_DEBUG, "Not setting window title for disabled output %s", wlr_output->name);
		return;
	}

	if (wlr_output_is_wl(wlr_output)) {
		wlr_wl_output_set_title(wlr_output, title);
#if WLR_HAS_X11_BACKEND
	} else if (wlr_output_is_x11(wlr_output)) {
		wlr_x11_output_set_title(wlr_output, title);
#endif
	}
}
