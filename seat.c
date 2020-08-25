/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "config.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_primary_selection.h>
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
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

static void drag_icon_update_position(struct cg_drag_icon *drag_icon);

/* XDG toplevels may have nested surfaces, such as popup windows for context
 * menus or tooltips. This function tests if any of those are underneath the
 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
 * surface pointer to that wlr_surface and the sx and sy coordinates to the
 * coordinates relative to that surface's top-left corner. */
static bool
view_at(struct cg_view *view, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	double view_sx = lx - view->lx;
	double view_sy = ly - view->ly;

	double _sx, _sy;
	struct wlr_surface *_surface = view_wlr_surface_at(view, view_sx, view_sy, &_sx, &_sy);
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
 * top-to-bottom. If desktop_view_at returns a view, there is also a
 * surface. There cannot be a surface without a view, either. It's
 * both or nothing. */
static struct cg_view *
desktop_view_at(struct cg_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	struct cg_view *view;

	wl_list_for_each (view, &server->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}

	return NULL;
}

static void
press_cursor_button(struct cg_seat *seat, struct wlr_input_device *device, uint32_t time, uint32_t button,
		    uint32_t state, double lx, double ly)
{
	struct cg_server *server = seat->server;

	if (state == WLR_BUTTON_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface;
		struct cg_view *view = desktop_view_at(server, lx, ly, &surface, &sx, &sy);
		struct cg_view *current = seat_get_focus(seat);
		if (view == current) {
			return;
		}

		/* Focus that client if the button was pressed and
		   it has no open dialogs. */
		if (view && !view_is_transient_for(current, view)) {
			seat_set_focus(seat, view);
		}
	}
}

static void
update_capabilities(struct cg_seat *seat)
{
	uint32_t caps = 0;

	if (!wl_list_empty(&seat->keyboard_groups)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	if (!wl_list_empty(&seat->pointers)) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	if (!wl_list_empty(&seat->touch)) {
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	}
	wlr_seat_set_capabilities(seat->seat, caps);

	/* Hide cursor if the seat doesn't have pointer capability. */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
		wlr_cursor_set_image(seat->cursor, NULL, 0, 0, 0, 0, 0, 0);
	} else {
		wlr_xcursor_manager_set_cursor_image(seat->xcursor_manager, DEFAULT_XCURSOR, seat->cursor);
	}
}

static void
map_input_device_to_output(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct cg_output *output;

	if (!device->output_name) {
		if (wl_list_length(&seat->server->outputs) == 1) {
			wl_list_for_each (output, &seat->server->outputs, link) {
				wlr_log(WLR_INFO, "Input device %s does not have an output name, mapping to only output %s\n", device->name,
					output->wlr_output->name);
				wlr_cursor_map_input_to_output(seat->cursor, device, output->wlr_output);
				return;
			}
		} else {
			wlr_log(WLR_INFO, "Input device %s cannot be mapped to an output device\n", device->name);
			return;
		}
	}

	wl_list_for_each (output, &seat->server->outputs, link) {
		if (strcmp(device->output_name, output->wlr_output->name) == 0) {
			wlr_log(WLR_INFO, "Mapping input device %s to output device %s\n", device->name,
				output->wlr_output->name);
			wlr_cursor_map_input_to_output(seat->cursor, device, output->wlr_output);
			return;
		}
	}

	wlr_log(WLR_INFO, "Couldn't map input device %s to an output\n", device->name);
}

static void
handle_touch_destroy(struct wl_listener *listener, void *data)
{
	struct cg_touch *touch = wl_container_of(listener, touch, destroy);
	struct cg_seat *seat = touch->seat;

	wl_list_remove(&touch->link);
	wlr_cursor_detach_input_device(seat->cursor, touch->device);
	wl_list_remove(&touch->destroy.link);
	free(touch);

	update_capabilities(seat);
}

static void
handle_new_touch(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct cg_touch *touch = calloc(1, sizeof(struct cg_touch));
	if (!touch) {
		wlr_log(WLR_ERROR, "Cannot allocate touch");
		return;
	}

	touch->seat = seat;
	touch->device = device;
	wlr_cursor_attach_input_device(seat->cursor, device);

	wl_list_insert(&seat->touch, &touch->link);
	touch->destroy.notify = handle_touch_destroy;
	wl_signal_add(&touch->device->events.destroy, &touch->destroy);

	map_input_device_to_output(seat, device);
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

	map_input_device_to_output(seat, device);
}

static void
handle_modifier_event(struct wlr_input_device *device, struct cg_seat *seat)
{
	wlr_seat_set_keyboard(seat->seat, device);
	wlr_seat_keyboard_notify_modifiers(seat->seat, &device->keyboard->modifiers);

	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static bool
handle_keybinding(struct cg_server *server, xkb_keysym_t sym)
{
#ifdef DEBUG
	if (sym == XKB_KEY_Escape) {
		wl_display_terminate(server->wl_display);
		return true;
	}
#endif
	if (server->allow_vt_switch && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (wlr_backend_is_multi(server->backend)) {
			struct wlr_session *session = wlr_backend_get_session(server->backend);
			if (session) {
				unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
				wlr_session_change_vt(session, vt);
			}
		}
	} else {
		return false;
	}
	wlr_idle_notify_activity(server->idle, server->seat->seat);
	return true;
}

static void
handle_key_event(struct wlr_input_device *device, struct cg_seat *seat, void *data)
{
	struct wlr_event_keyboard_key *event = data;

	/* Translate from libinput keycode to an xkbcommon keycode. */
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(device->keyboard);
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
		wlr_seat_set_keyboard(seat->seat, device);
		wlr_seat_keyboard_notify_key(seat->seat, event->time_msec, event->keycode, event->state);
	}

	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_keyboard_group_key(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *cg_group = wl_container_of(listener, cg_group, key);
	handle_key_event(cg_group->wlr_group->input_device, cg_group->seat, data);
}

static void
handle_keyboard_group_modifiers(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *group = wl_container_of(listener, group, modifiers);
	handle_modifier_event(group->wlr_group->input_device, group->seat);
}

static void
cg_keyboard_group_add(struct wlr_input_device *device, struct cg_seat *seat)
{
	struct wlr_keyboard *wlr_keyboard = device->keyboard;

	struct cg_keyboard_group *group;
	wl_list_for_each (group, &seat->keyboard_groups, link) {
		struct wlr_keyboard_group *wlr_group = group->wlr_group;
		if (wlr_keyboard_group_add_keyboard(wlr_group, wlr_keyboard)) {
			wlr_log(WLR_DEBUG, "Added new keyboard to existing group");
			return;
		}
	}

	/* This is reached if and only if the keyboard could not be inserted into
	 * any group */
	struct cg_keyboard_group *cg_group = calloc(1, sizeof(struct cg_keyboard_group));
	if (cg_group == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate keyboard group.");
		return;
	}
	cg_group->seat = seat;
	cg_group->wlr_group = wlr_keyboard_group_create();
	if (cg_group->wlr_group == NULL) {
		wlr_log(WLR_ERROR, "Failed to create wlr keyboard group.");
		goto cleanup;
	}

	cg_group->wlr_group->data = cg_group;
	wlr_keyboard_set_keymap(&cg_group->wlr_group->keyboard, device->keyboard->keymap);

	wlr_keyboard_set_repeat_info(&cg_group->wlr_group->keyboard, wlr_keyboard->repeat_info.rate,
				     wlr_keyboard->repeat_info.delay);

	wlr_log(WLR_DEBUG, "Created keyboard group");

	wlr_keyboard_group_add_keyboard(cg_group->wlr_group, wlr_keyboard);
	wl_list_insert(&seat->keyboard_groups, &cg_group->link);

	wl_signal_add(&cg_group->wlr_group->keyboard.events.key, &cg_group->key);
	cg_group->key.notify = handle_keyboard_group_key;
	wl_signal_add(&cg_group->wlr_group->keyboard.events.modifiers, &cg_group->modifiers);
	cg_group->modifiers.notify = handle_keyboard_group_modifiers;

	return;

cleanup:
	if (cg_group && cg_group->wlr_group) {
		wlr_keyboard_group_destroy(cg_group->wlr_group);
	}
	free(cg_group);
}

static void
handle_new_keyboard(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Unable to create XBK context");
		return;
	}

	struct xkb_rule_names rules = {0};
	rules.rules = getenv("XKB_DEFAULT_RULES");
	rules.model = getenv("XKB_DEFAULT_MODEL");
	rules.layout = getenv("XKB_DEFAULT_LAYOUT");
	rules.variant = getenv("XKB_DEFAULT_VARIANT");
	rules.options = getenv("XKB_DEFAULT_OPTIONS");
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unable to configure keyboard: keymap does not exist");
		xkb_context_unref(context);
		return;
	}

	wlr_keyboard_set_keymap(device->keyboard, keymap);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	cg_keyboard_group_add(device, seat);

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
		handle_new_touch(seat, device);
		break;
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
handle_request_set_primary_selection(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;

	wlr_seat_set_primary_selection(seat->seat, event->source, event->serial);
}

static void
handle_request_set_selection(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(seat->seat, event->source, event->serial);
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
		wlr_cursor_set_surface(seat->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void
handle_touch_down(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_down);
	struct wlr_event_touch_down *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, event->device, event->x, event->y, &lx, &ly);

	double sx, sy;
	struct wlr_surface *surface;
	struct cg_view *view = desktop_view_at(seat->server, lx, ly, &surface, &sx, &sy);

	uint32_t serial = 0;
	if (view) {
		serial = wlr_seat_touch_notify_down(seat->seat, surface, event->time_msec, event->touch_id, sx, sy);
	}

	if (serial && wlr_seat_touch_num_points(seat->seat) == 1) {
		seat->touch_id = event->touch_id;
		seat->touch_lx = lx;
		seat->touch_ly = ly;
		press_cursor_button(seat, event->device, event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED, lx, ly);
	}

	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_touch_up(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_up);
	struct wlr_event_touch_up *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	if (wlr_seat_touch_num_points(seat->seat) == 1) {
		press_cursor_button(seat, event->device, event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED,
				    seat->touch_lx, seat->touch_ly);
	}

	wlr_seat_touch_notify_up(seat->seat, event->time_msec, event->touch_id);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_motion);
	struct wlr_event_touch_motion *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, event->device, event->x, event->y, &lx, &ly);

	double sx, sy;
	struct wlr_surface *surface;
	struct cg_view *view = desktop_view_at(seat->server, lx, ly, &surface, &sx, &sy);

	if (view) {
		wlr_seat_touch_point_focus(seat->seat, surface, event->time_msec, event->touch_id, sx, sy);
		wlr_seat_touch_notify_motion(seat->seat, event->time_msec, event->touch_id, sx, sy);
	} else {
		wlr_seat_touch_point_clear_focus(seat->seat, event->time_msec, event->touch_id);
	}

	if (event->touch_id == seat->touch_id) {
		seat->touch_lx = lx;
		seat->touch_ly = ly;
	}

	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_frame(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_frame);

	wlr_seat_pointer_notify_frame(seat->seat);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_axis(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_axis);
	struct wlr_event_pointer_axis *event = data;

	wlr_seat_pointer_notify_axis(seat->seat, event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_button(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_button);
	struct wlr_event_pointer_button *event = data;

	wlr_seat_pointer_notify_button(seat->seat, event->time_msec, event->button, event->state);
	press_cursor_button(seat, event->device, event->time_msec, event->button, event->state, seat->cursor->x,
			    seat->cursor->y);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
process_cursor_motion(struct cg_seat *seat, uint32_t time)
{
	double sx, sy;
	struct wlr_seat *wlr_seat = seat->seat;
	struct wlr_surface *surface = NULL;

	struct cg_view *view = desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy);

	if (!view) {
		wlr_seat_pointer_clear_focus(wlr_seat);
	} else {
		wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);

		bool focus_changed = wlr_seat->pointer_state.focused_surface != surface;
		if (!focus_changed && time > 0) {
			wlr_seat_pointer_notify_motion(wlr_seat, time, sx, sy);
		}
	}

	struct cg_drag_icon *drag_icon;
	wl_list_for_each (drag_icon, &seat->drag_icons, link) {
		drag_icon_update_position(drag_icon);
	}

	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;

	wlr_cursor_warp_absolute(seat->cursor, event->device, event->x, event->y);
	process_cursor_motion(seat, event->time_msec);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_motion(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion);
	struct wlr_event_pointer_motion *event = data;

	wlr_cursor_move(seat->cursor, event->device, event->delta_x, event->delta_y);
	process_cursor_motion(seat, event->time_msec);
	wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

static void
drag_icon_damage(struct cg_drag_icon *drag_icon)
{
	struct cg_output *output;
	wl_list_for_each (output, &drag_icon->seat->server->outputs, link) {
		output_damage_surface(output, drag_icon->wlr_drag_icon->surface, drag_icon->lx, drag_icon->ly, true);
	}
}

static void
drag_icon_update_position(struct cg_drag_icon *drag_icon)
{
	struct wlr_drag_icon *wlr_icon = drag_icon->wlr_drag_icon;
	struct cg_seat *seat = drag_icon->seat;
	struct wlr_touch_point *point;

	drag_icon_damage(drag_icon);

	switch (wlr_icon->drag->grab_type) {
	case WLR_DRAG_GRAB_KEYBOARD:
		return;
	case WLR_DRAG_GRAB_KEYBOARD_POINTER:
		drag_icon->lx = seat->cursor->x;
		drag_icon->ly = seat->cursor->y;
		break;
	case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
		point = wlr_seat_touch_get_point(seat->seat, wlr_icon->drag->touch_id);
		if (!point) {
			return;
		}
		drag_icon->lx = seat->touch_lx;
		drag_icon->ly = seat->touch_ly;
		break;
	}

	drag_icon_damage(drag_icon);
}

static void
handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
	struct cg_drag_icon *drag_icon = wl_container_of(listener, drag_icon, destroy);

	wl_list_remove(&drag_icon->link);
	wl_list_remove(&drag_icon->destroy.link);
	free(drag_icon);
}

static void
handle_request_start_drag(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(seat->seat, event->drag, event->serial);
		return;
	}

	struct wlr_touch_point *point;
	if (wlr_seat_validate_touch_grab_serial(seat->seat, event->origin, event->serial, &point)) {
		wlr_seat_start_touch_drag(seat->seat, event->drag, event->serial, point);
		return;
	}

	// TODO: tablet grabs
	wlr_log(WLR_DEBUG, "Ignoring start_drag request: could not validate pointer/touch serial %" PRIu32,
		event->serial);
	wlr_data_source_destroy(event->drag->source);
}

static void
handle_start_drag(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, start_drag);
	struct wlr_drag *wlr_drag = data;
	struct wlr_drag_icon *wlr_drag_icon = wlr_drag->icon;
	if (wlr_drag_icon == NULL) {
		return;
	}

	struct cg_drag_icon *drag_icon = calloc(1, sizeof(struct cg_drag_icon));
	if (!drag_icon) {
		return;
	}
	drag_icon->seat = seat;
	drag_icon->wlr_drag_icon = wlr_drag_icon;

	drag_icon->destroy.notify = handle_drag_icon_destroy;
	wl_signal_add(&wlr_drag_icon->events.destroy, &drag_icon->destroy);

	wl_list_insert(&seat->drag_icons, &drag_icon->link);

	drag_icon_update_position(drag_icon);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, destroy);
	wl_list_remove(&seat->destroy.link);
	wl_list_remove(&seat->cursor_motion.link);
	wl_list_remove(&seat->cursor_motion_absolute.link);
	wl_list_remove(&seat->cursor_button.link);
	wl_list_remove(&seat->cursor_axis.link);
	wl_list_remove(&seat->cursor_frame.link);
	wl_list_remove(&seat->touch_down.link);
	wl_list_remove(&seat->touch_up.link);
	wl_list_remove(&seat->touch_motion.link);
	wl_list_remove(&seat->request_set_cursor.link);
	wl_list_remove(&seat->request_set_selection.link);
	wl_list_remove(&seat->request_set_primary_selection.link);

	struct cg_keyboard_group *group, *group_tmp;
	wl_list_for_each_safe (group, group_tmp, &seat->keyboard_groups, link) {
		wlr_keyboard_group_destroy(group->wlr_group);
		free(group);
	}
	struct cg_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe (pointer, pointer_tmp, &seat->pointers, link) {
		handle_pointer_destroy(&pointer->destroy, NULL);
	}
	struct cg_touch *touch, *touch_tmp;
	wl_list_for_each_safe (touch, touch_tmp, &seat->touch, link) {
		handle_touch_destroy(&touch->destroy, NULL);
	}
	wl_list_remove(&seat->new_input.link);

	wlr_xcursor_manager_destroy(seat->xcursor_manager);
	if (seat->cursor) {
		wlr_cursor_destroy(seat->cursor);
	}
	free(seat);
}

struct cg_seat *
seat_create(struct cg_server *server, struct wlr_backend *backend)
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
	seat->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&seat->cursor->events.frame, &seat->cursor_frame);

	seat->touch_down.notify = handle_touch_down;
	wl_signal_add(&seat->cursor->events.touch_down, &seat->touch_down);
	seat->touch_up.notify = handle_touch_up;
	wl_signal_add(&seat->cursor->events.touch_up, &seat->touch_up);
	seat->touch_motion.notify = handle_touch_motion;
	wl_signal_add(&seat->cursor->events.touch_motion, &seat->touch_motion);

	seat->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&seat->seat->events.request_set_cursor, &seat->request_set_cursor);
	seat->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&seat->seat->events.request_set_selection, &seat->request_set_selection);
	seat->request_set_primary_selection.notify = handle_request_set_primary_selection;
	wl_signal_add(&seat->seat->events.request_set_primary_selection, &seat->request_set_primary_selection);

	wl_list_init(&seat->keyboards);
	wl_list_init(&seat->keyboard_groups);
	wl_list_init(&seat->pointers);
	wl_list_init(&seat->touch);

	seat->new_input.notify = handle_new_input;
	wl_signal_add(&backend->events.new_input, &seat->new_input);

	wl_list_init(&seat->drag_icons);
	seat->request_start_drag.notify = handle_request_start_drag;
	wl_signal_add(&seat->seat->events.request_start_drag, &seat->request_start_drag);
	seat->start_drag.notify = handle_start_drag;
	wl_signal_add(&seat->seat->events.start_drag, &seat->start_drag);

	return seat;
}

void
seat_destroy(struct cg_seat *seat)
{
	if (!seat) {
		return;
	}

	wl_list_remove(&seat->request_start_drag.link);
	wl_list_remove(&seat->start_drag.link);

	// Destroying the wlr seat will trigger the destroy handler on our seat,
	// which will in turn free it.
	wlr_seat_destroy(seat->seat);
}

struct cg_view *
seat_get_focus(struct cg_seat *seat)
{
	struct wlr_surface *prev_surface = seat->seat->keyboard_state.focused_surface;
	return view_from_wlr_surface(seat->server, prev_surface);
}

void
seat_set_focus(struct cg_seat *seat, struct cg_view *view)
{
	struct cg_server *server = seat->server;
	struct wlr_seat *wlr_seat = seat->seat;
	struct cg_view *prev_view = seat_get_focus(seat);

	if (!view || prev_view == view) {
		return;
	}

#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW) {
		struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
		if (!wlr_xwayland_or_surface_wants_focus(xwayland_view->xwayland_surface)) {
			return;
		}
	}
#endif

	if (prev_view) {
		view_activate(prev_view, false);
	}

	/* Move the view to the front, but only if it isn't a
	   fullscreen view. */
	if (!view_is_primary(view)) {
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
	}

	view_activate(view, true);
	char *title = view_get_title(view);
	struct cg_output *output;
	wl_list_for_each (output, &server->outputs, link) {
		output_set_window_title(output, title);
	}
	free(title);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(wlr_seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, keyboard->keycodes, keyboard->num_keycodes,
					       &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, NULL, 0, NULL);
	}

	process_cursor_motion(seat, -1);
}
