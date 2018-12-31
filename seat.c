/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "config.h"

#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"

static inline bool
have_dialogs_open(struct cg_server *server)
{
	/* We only need to test if there is more than a single
	   element. We don't need to know the entire length of the
	   list. */
	return server->views.next != server->views.prev;
}

/* XDG toplevels may have nested surfaces, such as popup windows for context
 * menus or tooltips. This function tests if any of those are underneath the
 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
 * surface pointer to that wlr_surface and the sx and sy coordinates to the
 * coordinates relative to that surface's top-left corner. */
static bool
view_at(struct cg_view *view, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy)
{
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
	switch (view->type) {
	case CAGE_XDG_SHELL_VIEW:
		_surface = wlr_xdg_surface_surface_at(view->xdg_surface,
						      view_sx, view_sy,
						      &_sx, &_sy);
		break;
#ifdef CAGE_HAS_XWAYLAND
	case CAGE_XWAYLAND_VIEW:
		_surface = wlr_surface_surface_at(view->wlr_surface,
						  view_sx, view_sy,
						  &_sx, &_sy);
		break;
#endif
	default:
		wlr_log(WLR_ERROR, "Unrecognized view type: %d", view->type);
	}


	if (_surface != NULL) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}

	return false;
}

/* This iterates over all of our surfaces and attempts to find one
 * under the cursor. This relies on server->views being ordered from
 * top-to-bottom. */
static struct cg_view *
desktop_view_at(struct cg_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy)
{
	struct cg_view *view;

	wl_list_for_each(view, &server->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}

	return NULL;
}

static void
update_capabilities(struct cg_seat *seat) {
	uint32_t caps = 0;

	if (!wl_list_empty(&seat->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	if (!wl_list_empty(&seat->pointers)) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	wlr_seat_set_capabilities(seat->seat, caps);

	/* Hide cursor if the seat doesn't have pointer capability. */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
		wlr_cursor_set_image(seat->cursor, NULL, 0, 0, 0, 0, 0, 0);
	} else {
		wlr_xcursor_manager_set_cursor_image(seat->xcursor_manager,
						     DEFAULT_XCURSOR,
						     seat->cursor);
	}
}

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct cg_pointer *pointer = wl_container_of(listener, pointer, destroy);
	struct cg_seat *seat = pointer->seat;

	wl_list_remove(&pointer->link);
	wlr_cursor_detach_input_device(seat->cursor, pointer->device);
	wl_list_remove(&pointer->destroy.link);
	free(pointer);

	update_capabilities(seat);
}

static void
handle_new_pointer(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct cg_pointer *pointer = calloc(1, sizeof(struct cg_pointer));
	if (!pointer) {
		wlr_log(WLR_ERROR, "Cannot allocate pointer");
		return;
	}

	pointer->seat = seat;
	pointer->device = device;
	wlr_cursor_attach_input_device(seat->cursor, device);

	wl_list_insert(&seat->pointers, &pointer->link);
	pointer->destroy.notify = handle_pointer_destroy;
	wl_signal_add(&device->events.destroy, &pointer->destroy);
}

static void
handle_keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

	wlr_seat_set_keyboard(keyboard->seat->seat, keyboard->device);
	wlr_seat_keyboard_notify_modifiers(keyboard->seat->seat,
					   &keyboard->device->keyboard->modifiers);
}

static bool
handle_keybinding(struct cg_server *server, xkb_keysym_t sym)
{
	switch (sym) {
#ifdef DEBUG
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
#endif
	default:
		return false;
	}
	return true;
}

static void
handle_keyboard_key(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct cg_seat *seat = keyboard->seat;
	struct wlr_event_keyboard_key *event = data;

	/* Translate from libinput keycode to an xkbcommon keycode. */
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WLR_KEY_PRESSED) {
		/* If Alt is held down and this button was pressed, we
		 * attempt to process it as a compositor
		 * keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(seat->server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat->seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat->seat, event->time_msec,
					     event->keycode, event->state);
	}
}

static void
handle_keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct cg_seat *seat = keyboard->seat;

	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);

	update_capabilities(seat);
}

static void
handle_new_keyboard(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct cg_keyboard *keyboard = calloc(1, sizeof(struct cg_keyboard));
	if (!keyboard) {
		wlr_log(WLR_ERROR, "Cannot allocate keyboard");
		return;
	}

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Unable to create XBK context");
		free(keyboard);
		return;
	}

	struct xkb_rule_names rules = { 0 };
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
							   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unable to configure keyboard: keymap does not exist");
		free(keyboard);
		xkb_context_unref(context);
		return;
	}

	keyboard->seat = seat;
	keyboard->device = device;
	wlr_keyboard_set_keymap(device->keyboard, keymap);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	wl_list_insert(&seat->keyboards, &keyboard->link);
	keyboard->destroy.notify = handle_keyboard_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);
	keyboard->key.notify = handle_keyboard_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);
	keyboard->modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);

	wlr_seat_set_keyboard(seat->seat, device);
}

static void
handle_new_input(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		handle_new_keyboard(seat, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		handle_new_pointer(seat, device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_log(WLR_DEBUG, "Touch input is not implemented");
		return;
	case WLR_INPUT_DEVICE_SWITCH:
		wlr_log(WLR_DEBUG, "Switch input is not implemented");
		return;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
	case WLR_INPUT_DEVICE_TABLET_PAD:
		wlr_log(WLR_DEBUG, "Tablet input is not implemented");
		return;
	}

	update_capabilities(seat);
}

static void
handle_request_set_cursor(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_surface *focused_surface = event->seat_client->seat->pointer_state.focused_surface;
	bool has_focused = focused_surface != NULL && focused_surface->resource != NULL;
	struct wl_client *focused_client = NULL;
	if (has_focused) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (focused_client == event->seat_client->client) {
		wlr_cursor_set_surface(seat->cursor, event->surface,
				       event->hotspot_x, event->hotspot_y);
	}
}

static void
handle_cursor_axis(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_axis);
	struct wlr_event_pointer_axis *event = data;

	wlr_seat_pointer_notify_axis(seat->seat,
				     event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source);
}

static void
handle_cursor_button(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_button);
	struct cg_server *server = seat->server;
	struct wlr_event_pointer_button *event = data;

	wlr_seat_pointer_notify_button(seat->seat,
				       event->time_msec, event->button, event->state);
	if (event->state == WLR_BUTTON_PRESSED && !have_dialogs_open(server)) {
		/* Focus that client if the button was pressed and
		   there are no open dialogs. */
		double sx, sy;
		struct wlr_surface *surface;
		struct cg_view *view = desktop_view_at(server,
						       seat->cursor->x,
						       seat->cursor->y,
						       &surface, &sx, &sy);
		if (view) {
			seat_set_focus(seat, view);
		}
	}
}

static void
process_cursor_motion(struct cg_seat *seat, uint32_t time)
{
	double sx, sy;
	struct wlr_seat *wlr_seat = seat->seat;
	struct wlr_surface *surface = NULL;

	struct cg_view *view = desktop_view_at(seat->server,
					       seat->cursor->x, seat->cursor->y,
					       &surface, &sx, &sy);

	/* If desktop_view_at returns a view, there is also a
	   surface. There cannot be a surface without a view,
	   either. It's both or nothing. */
	if (!view) {
		wlr_seat_pointer_clear_focus(wlr_seat);
	} else {
		wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);

		bool focus_changed = wlr_seat->pointer_state.focused_surface != surface;
		if (!focus_changed && time > 0) {
			wlr_seat_pointer_notify_motion(wlr_seat, time, sx, sy);
		}
	}
}

static void
handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;

	wlr_cursor_warp_absolute(seat->cursor, event->device, event->x, event->y);
	process_cursor_motion(seat, event->time_msec);
}

static void
handle_cursor_motion(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion);
	struct wlr_event_pointer_motion *event = data;

	wlr_cursor_move(seat->cursor, event->device, event->delta_x, event->delta_y);
	process_cursor_motion(seat, event->time_msec);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, destroy);
	wl_list_remove(&seat->destroy.link);

	struct cg_keyboard *keyboard, *keyboard_tmp;
	wl_list_for_each_safe(keyboard, keyboard_tmp, &seat->keyboards, link) {
		handle_keyboard_destroy(&keyboard->destroy, NULL);
	}
	struct cg_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe(pointer, pointer_tmp, &seat->pointers, link) {
		handle_pointer_destroy(&pointer->destroy, NULL);
	}
	wl_list_remove(&seat->new_input.link);

	wlr_xcursor_manager_destroy(seat->xcursor_manager);
	if (seat->cursor) {
		wlr_cursor_destroy(seat->cursor);
	}
	wl_list_remove(&seat->cursor_motion.link);
	wl_list_remove(&seat->cursor_motion_absolute.link);
	wl_list_remove(&seat->cursor_button.link);
	wl_list_remove(&seat->cursor_axis.link);
	wl_list_remove(&seat->request_set_cursor.link);
}

struct cg_seat *
cg_seat_create(struct cg_server *server)
{
	struct cg_seat *seat = calloc(1, sizeof(struct cg_seat));
	if (!seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat");
		return NULL;
	}

	seat->seat = wlr_seat_create(server->wl_display, "seat0");
	if (!seat->seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat0");
		free(seat);
		return NULL;
	}
	seat->server = server;
	seat->destroy.notify = handle_destroy;
	wl_signal_add(&seat->seat->events.destroy, &seat->destroy);

	seat->cursor = wlr_cursor_create();
	if (!seat->cursor) {
		wlr_log(WLR_ERROR, "Unable to create cursor");
		wl_list_remove(&seat->destroy.link);
		free(seat);
		return NULL;
	}
	wlr_cursor_attach_output_layout(seat->cursor, server->output_layout);

	if (!seat->xcursor_manager) {
		seat->xcursor_manager = wlr_xcursor_manager_create(NULL, XCURSOR_SIZE);
		if (!seat->xcursor_manager) {
			wlr_log(WLR_ERROR, "Cannot create XCursor manager");
			wlr_cursor_destroy(seat->cursor);
			wl_list_remove(&seat->destroy.link);
			free(seat);
			return NULL;
		}
	}

	seat->cursor_motion.notify = handle_cursor_motion;
	wl_signal_add(&seat->cursor->events.motion, &seat->cursor_motion);
	seat->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&seat->cursor->events.motion_absolute, &seat->cursor_motion_absolute);
	seat->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&seat->cursor->events.button, &seat->cursor_button);
	seat->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&seat->cursor->events.axis, &seat->cursor_axis);

	seat->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&seat->seat->events.request_set_cursor, &seat->request_set_cursor);

	wl_list_init(&seat->keyboards);
	wl_list_init(&seat->pointers);

	seat->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &seat->new_input);

	return seat;
}

void
cg_seat_destroy(struct cg_seat *seat)
{
	if (!seat) {
		return;
	}

	handle_destroy(&seat->destroy, NULL);
	wlr_seat_destroy(seat->seat);
}

struct cg_view *
seat_get_focus(struct cg_seat *seat)
{
	struct wlr_surface *prev_surface = seat->seat->keyboard_state.focused_surface;
	return cg_view_from_wlr_surface(seat->server, prev_surface);
}

void
seat_set_focus(struct cg_seat *seat, struct cg_view *view)
{
	struct cg_server *server = seat->server;
	struct wlr_seat *wlr_seat = seat->seat;
	struct cg_view *prev_view = seat_get_focus(seat);

	if (prev_view == view || !view) {
		return;
	}

#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW &&
	    !wlr_xwayland_or_surface_wants_focus(view->xwayland_surface)) {
		return;
	}
#endif

	if (prev_view) {
		view_activate(prev_view, false);
	}

	/* Move the view to the front, but only if it isn't the
	   fullscreen view. */
	if (!view_is_primary(view)) {
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
	}

        view_activate(view, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(wlr_seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface,
					       keyboard->keycodes,
					       keyboard->num_keycodes,
					       &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface,
					       NULL, 0, NULL);
	}

	process_cursor_motion(seat, -1);
}
