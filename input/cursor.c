/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "cursor.h"

static void
handle_surface_destroy(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, request_set_cursor);
	cage_cursor_set_image(cursor, DEFAULT_XCURSOR);
}

static void
handle_request_set_cursor(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = user_data;

	struct wlr_surface *focused_surface = cursor->wlr_seat->pointer_state.focused_surface;
	bool has_focused = focused_surface != NULL && focused_surface->resource != NULL;
	struct wl_client *focused_client = NULL;
	if (has_focused) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (focused_client == NULL || focused_client != event->seat_client->client) {
		return;
	}

	wl_list_remove(&cursor->surface_destroy.link);
	if (event->surface != NULL) {
		cursor->surface_destroy.notify = handle_surface_destroy;
		wl_signal_add(&event->surface->events.destroy, &cursor->surface_destroy);
	} else {
		wl_list_init(&cursor->surface_destroy.link);
	}

	wlr_cursor_set_surface(cursor->wlr_cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void
handle_cursor_motion(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, cursor_motion);
	struct wlr_event_pointer_motion *event = user_data;

	wlr_cursor_move(cursor->wlr_cursor, event->device, event->delta_x, event->delta_y);
	wl_signal_emit(&cursor->events.motion, &event->time_msec);
}

static void
handle_cursor_motion_absolute(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = user_data;

	wlr_cursor_warp_absolute(cursor->wlr_cursor, event->device, event->x, event->y);
	wl_signal_emit(&cursor->events.motion, &event->time_msec);
}

static void
handle_cursor_button(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, cursor_button);
	struct wlr_event_pointer_button *event = user_data;

	wlr_seat_pointer_notify_button(cursor->wlr_seat, event->time_msec, event->button, event->state);
	wl_signal_emit(&cursor->events.button, user_data);
}

static void
handle_cursor_axis(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, cursor_axis);
	struct wlr_event_pointer_axis *event = user_data;

	wlr_seat_pointer_notify_axis(cursor->wlr_seat, event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source);
}

static void
handle_cursor_frame(struct wl_listener *listener, void *user_data)
{
	struct cg_cursor *cursor = wl_container_of(listener, cursor, cursor_frame);

	wlr_seat_pointer_notify_frame(cursor->wlr_seat);
}

void
cage_cursor_set_image(struct cg_cursor *cursor, const char *path)
{
	assert(cursor != NULL);

	wl_list_remove(&cursor->surface_destroy.link);
	wl_list_init(&cursor->surface_destroy.link);

	if (path != NULL) {
		wlr_xcursor_manager_set_cursor_image(cursor->xcursor_manager, path, cursor->wlr_cursor);
	} else {
		wlr_cursor_set_image(cursor->wlr_cursor, NULL, 0, 0, 0, 0, 0, 0);
	}
}

void
cage_cursor_init(struct cg_cursor *cursor, struct wlr_cursor *wlr_cursor, struct wlr_xcursor_manager *xcursor_manager,
		 struct wlr_seat *wlr_seat)
{
	assert(cursor != NULL);
	assert(wlr_cursor != NULL);
	assert(xcursor_manager != NULL);

	cursor->wlr_cursor = wlr_cursor;
	cursor->xcursor_manager = xcursor_manager;
	cursor->wlr_seat = wlr_seat;

	cursor->cursor_motion.notify = handle_cursor_motion;
	wl_signal_add(&wlr_cursor->events.motion, &cursor->cursor_motion);
	cursor->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&wlr_cursor->events.motion_absolute, &cursor->cursor_motion_absolute);
	cursor->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&wlr_cursor->events.button, &cursor->cursor_button);
	cursor->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&wlr_cursor->events.axis, &cursor->cursor_axis);
	cursor->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&wlr_cursor->events.frame, &cursor->cursor_frame);

	wl_signal_init(&cursor->events.motion);
	wl_signal_init(&cursor->events.button);

	cursor->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&wlr_seat->events.request_set_cursor, &cursor->request_set_cursor);
	wl_list_init(&cursor->surface_destroy.link);

	cage_cursor_set_image(cursor, DEFAULT_XCURSOR);
}

void
cage_cursor_fini(struct cg_cursor *cursor)
{
	assert(cursor != NULL);

	wlr_cursor_destroy(cursor->wlr_cursor);
	wlr_xcursor_manager_destroy(cursor->xcursor_manager);
	cursor->wlr_seat = NULL;

	wl_list_remove(&cursor->cursor_motion.link);
	wl_list_remove(&cursor->cursor_motion_absolute.link);
	wl_list_remove(&cursor->cursor_button.link);
	wl_list_remove(&cursor->cursor_axis.link);
	wl_list_remove(&cursor->cursor_frame.link);

	wl_list_remove(&cursor->request_set_cursor.link);

	cage_cursor_set_image(cursor, NULL);
	free(cursor);
}
