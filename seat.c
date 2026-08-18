/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
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
 * coordinates relative to that surface's top-left corner.
 *
 * This function iterates over all of our surfaces and attempts to find one
 * under the cursor. If desktop_view_at returns a view, there is also a
 * surface. There cannot be a surface without a view, either. It's both or
 * nothing.
 */
static struct cg_view *
desktop_view_at(struct cg_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;

	/* Walk up the tree until we find a node with a data pointer. When done,
	 * we've found the node representing the view. */
	while (!node->data) {
		if (!node->parent) {
			node = NULL;
			break;
		}

		node = &node->parent->node;
	}

	assert(node != NULL);
	return node->data;
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
		wlr_cursor_unset_image(seat->cursor);
	} else {
		wlr_cursor_set_xcursor(seat->cursor, seat->server->xcursor_manager, DEFAULT_XCURSOR);
	}
}

static void
map_input_device_to_output(struct cg_seat *seat, struct wlr_input_device *device, const char *output_name)
{
	if (!output_name) {
		wlr_log(WLR_INFO, "Input device %s cannot be mapped to an output device\n", device->name);
		return;
	}

	struct cg_output *output;
	wl_list_for_each (output, &seat->server->outputs, link) {
		if (strcmp(output_name, output->wlr_output->name) == 0) {
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
	wlr_cursor_detach_input_device(seat->cursor, &touch->touch->base);
	wl_list_remove(&touch->destroy.link);
	free(touch);

	update_capabilities(seat);
}

static void
handle_new_touch(struct cg_seat *seat, struct wlr_touch *wlr_touch)
{
	struct cg_touch *touch = calloc(1, sizeof(struct cg_touch));
	if (!touch) {
		wlr_log(WLR_ERROR, "Cannot allocate touch");
		return;
	}

	touch->seat = seat;
	touch->touch = wlr_touch;
	wlr_cursor_attach_input_device(seat->cursor, &wlr_touch->base);

	wl_list_insert(&seat->touch, &touch->link);
	touch->destroy.notify = handle_touch_destroy;
	wl_signal_add(&wlr_touch->base.events.destroy, &touch->destroy);

	map_input_device_to_output(seat, &wlr_touch->base, wlr_touch->output_name);
}

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct cg_pointer *pointer = wl_container_of(listener, pointer, destroy);
	struct cg_seat *seat = pointer->seat;

	wl_list_remove(&pointer->link);
	wlr_cursor_detach_input_device(seat->cursor, &pointer->pointer->base);
	wl_list_remove(&pointer->destroy.link);
	free(pointer);

	update_capabilities(seat);
}

static void
handle_new_pointer(struct cg_seat *seat, struct wlr_pointer *wlr_pointer)
{
	struct cg_pointer *pointer = calloc(1, sizeof(struct cg_pointer));
	if (!pointer) {
		wlr_log(WLR_ERROR, "Cannot allocate pointer");
		return;
	}

	pointer->seat = seat;
	pointer->pointer = wlr_pointer;
	wlr_cursor_attach_input_device(seat->cursor, &wlr_pointer->base);

	wl_list_insert(&seat->pointers, &pointer->link);
	pointer->destroy.notify = handle_pointer_destroy;
	wl_signal_add(&wlr_pointer->base.events.destroy, &pointer->destroy);

	map_input_device_to_output(seat, &wlr_pointer->base, wlr_pointer->output_name);
}

static struct cg_pointer *
pointer_from_wlr_pointer(struct cg_seat *seat, struct wlr_pointer *wlr_pointer)
{
	struct cg_pointer *pointer;
	wl_list_for_each (pointer, &seat->pointers, link) {
		if (pointer->pointer == wlr_pointer) {
			return pointer;
		}
	}
	return NULL;
}

static void
handle_virtual_pointer(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct cg_seat *seat = server->seat;
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *pointer = event->new_pointer;
	struct wlr_pointer *wlr_pointer = &pointer->pointer;

	/* We'll want to map the device back to an output later, this is a bit
	 * sub-optimal (we could just keep the suggested_output), but just copy
	 * its name so we do like other devices
	 */
	if (event->suggested_output != NULL) {
		wlr_pointer->output_name = strdup(event->suggested_output->name);
	}
	/* TODO: event->suggested_seat should be checked if we handle multiple seats */
	handle_new_pointer(seat, wlr_pointer);
	update_capabilities(seat);
}

static void
handle_modifier_event(struct wlr_keyboard *keyboard, struct cg_seat *seat)
{
	wlr_seat_set_keyboard(seat->seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(seat->seat, &keyboard->modifiers);

	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static bool
handle_keybinding(struct cg_server *server, xkb_keysym_t sym)
{
#ifdef DEBUG
	if (sym == XKB_KEY_Escape) {
		server_terminate(server);
		return true;
	}
#endif
	if (server->allow_vt_switch && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (wlr_backend_is_multi(server->backend)) {
			if (server->session) {
				unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
				wlr_session_change_vt(server->session, vt);
			}
		}
	} else {
		return false;
	}
	wlr_idle_notifier_v1_notify_activity(server->idle, server->seat->seat);
	return true;
}

static void
handle_key_event(struct wlr_keyboard *keyboard, struct cg_seat *seat, void *data)
{
	struct wlr_keyboard_key_event *event = data;

	/* Translate from libinput keycode to an xkbcommon keycode. */
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If Alt is held down and this button was pressed, we
		 * attempt to process it as a compositor
		 * keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(seat->server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat->seat, keyboard);
		wlr_seat_keyboard_notify_key(seat->seat, event->time_msec, event->keycode, event->state);
	}

	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_keyboard_group_key(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *cg_group = wl_container_of(listener, cg_group, key);
	handle_key_event(&cg_group->wlr_group->keyboard, cg_group->seat, data);
}

static void
handle_keyboard_group_modifiers(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *group = wl_container_of(listener, group, modifiers);
	handle_modifier_event(&group->wlr_group->keyboard, group->seat);
}

static void
cg_keyboard_group_add(struct wlr_keyboard *keyboard, struct cg_seat *seat, bool virtual)
{
	/* We apparently should not group virtual keyboards,
	 * so create a new group with it
	 */
	if (!virtual) {
		struct cg_keyboard_group *group;
		wl_list_for_each (group, &seat->keyboard_groups, link) {
			if (group->is_virtual)
				continue;
			struct wlr_keyboard_group *wlr_group = group->wlr_group;
			if (wlr_keyboard_group_add_keyboard(wlr_group, keyboard)) {
				wlr_log(WLR_DEBUG, "Added new keyboard to existing group");
				return;
			}
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
	cg_group->is_virtual = virtual;
	cg_group->wlr_group = wlr_keyboard_group_create();
	if (cg_group->wlr_group == NULL) {
		wlr_log(WLR_ERROR, "Failed to create wlr keyboard group.");
		goto cleanup;
	}

	cg_group->wlr_group->data = cg_group;
	wlr_keyboard_set_keymap(&cg_group->wlr_group->keyboard, keyboard->keymap);

	wlr_keyboard_set_repeat_info(&cg_group->wlr_group->keyboard, keyboard->repeat_info.rate,
				     keyboard->repeat_info.delay);

	wlr_log(WLR_DEBUG, "Created keyboard group");

	wlr_keyboard_group_add_keyboard(cg_group->wlr_group, keyboard);
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
keyboard_group_destroy(struct cg_keyboard_group *keyboard_group)
{
	wl_list_remove(&keyboard_group->key.link);
	wl_list_remove(&keyboard_group->modifiers.link);
	wlr_keyboard_group_destroy(keyboard_group->wlr_group);
	wl_list_remove(&keyboard_group->link);
	free(keyboard_group);
}

static void
handle_new_keyboard(struct cg_seat *seat, struct wlr_keyboard *keyboard, bool virtual)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Unable to create XKB context");
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unable to configure keyboard: keymap does not exist");
		xkb_context_unref(context);
		return;
	}

	wlr_keyboard_set_keymap(keyboard, keymap);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, 25, 600);

	cg_keyboard_group_add(keyboard, seat, virtual);

	wlr_seat_set_keyboard(seat->seat, keyboard);
}

static void
handle_virtual_keyboard(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct cg_seat *seat = server->seat;
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	struct wlr_keyboard *wlr_keyboard = &keyboard->keyboard;

	/* TODO: If multiple seats are supported, check keyboard->seat
	 * to select the appropriate one */

	handle_new_keyboard(seat, wlr_keyboard, true);
	update_capabilities(seat);
}

static void
handle_new_input(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		handle_new_keyboard(seat, wlr_keyboard_from_input_device(device), false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		handle_new_pointer(seat, wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		handle_new_touch(seat, wlr_touch_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		wlr_log(WLR_DEBUG, "Switch input is not implemented");
		return;
	case WLR_INPUT_DEVICE_TABLET:
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

static bool
client_has_pointer_focus(struct cg_seat *seat, struct wl_client *client)
{
	struct wlr_surface *focused_surface = seat->seat->pointer_state.focused_surface;
	bool has_focused = focused_surface != NULL && focused_surface->resource != NULL;
	struct wl_client *focused_client = NULL;
	if (has_focused) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	return focused_client == client;
}

static void
handle_request_set_cursor(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (client_has_pointer_focus(seat, event->seat_client->client) &&
	    (seat->seat->capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
		wlr_cursor_set_surface(seat->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

void
handle_request_set_shape(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_shape_manager_set_shape);
	struct cg_seat *seat = server->seat;
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (client_has_pointer_focus(seat, event->seat_client->client) &&
	    (seat->seat->capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
		const char *shape_name = wlr_cursor_shape_v1_name(event->shape);
		wlr_cursor_set_xcursor(seat->cursor, seat->server->xcursor_manager, shape_name);
	}
}

static void
handle_touch_down(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_down);
	struct wlr_touch_down_event *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->touch->base, event->x, event->y, &lx, &ly);

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
		press_cursor_button(seat, &event->touch->base, event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED, lx, ly);
	}

	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_touch_up(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_up);
	struct wlr_touch_up_event *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	if (wlr_seat_touch_num_points(seat->seat) == 1) {
		press_cursor_button(seat, &event->touch->base, event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED,
				    seat->touch_lx, seat->touch_ly);
	}

	wlr_seat_touch_notify_up(seat->seat, event->time_msec, event->touch_id);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_motion);
	struct wlr_touch_motion_event *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->touch->base, event->x, event->y, &lx, &ly);

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

	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_touch_frame(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_frame);

	wlr_seat_touch_notify_frame(seat->seat);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_frame(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_frame);

	wlr_seat_pointer_notify_frame(seat->seat);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_axis(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	wlr_seat_pointer_notify_axis(seat->seat, event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source, event->relative_direction);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_button(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_seat_pointer_notify_button(seat->seat, event->time_msec, event->button, event->state);
	press_cursor_button(seat, &event->pointer->base, event->time_msec, event->button, event->state, seat->cursor->x,
			    seat->cursor->y);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

/* Pointer constraints (zwp_pointer_constraints_v1): clients — games, or
 * nested compositors on their behalf — ask for the pointer to be locked in
 * place or confined to a region of their surface. The constraint of the
 * pointer-focused surface is enforced: a locked pointer pins the cursor
 * (clients follow the zwp_relative_pointer_v1 stream instead), a confined
 * pointer is clamped to the constraint region. The active constraint always
 * belongs to the pointer-focused surface — it is updated on every focus
 * change — so the motion handlers below need not re-check the focus.
 *
 * A tracked constraint is not necessarily in effect. The protocol guarantees
 * the pointer is inside the constraint region whenever the client is told the
 * constraint activated, which an empty region cannot satisfy, so the
 * constraint is only enforced while seat->constraint_enforced is set. */

/* The scene node showing a surface, or NULL if it is not on screen. */
static struct wlr_scene_node *
scene_node_for_surface(struct wlr_scene_node *node, struct wlr_surface *surface)
{
	switch (node->type) {
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		return scene_surface && scene_surface->surface == surface ? node : NULL;
	}
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_node *child;
		wl_list_for_each (child, &wlr_scene_tree_from_node(node)->children, link) {
			struct wlr_scene_node *found = scene_node_for_surface(child, surface);
			if (found) {
				return found;
			}
		}
		return NULL;
	}
	default:
		return NULL;
	}
}

/* The cursor position in the coordinates of a constrained surface. Looked up
 * in the scene graph rather than derived from the seat's surface-local
 * position, which belongs to the focused surface — not necessarily this one —
 * and rather than cached at activation, which goes stale as soon as the view
 * is repositioned (an output is hotplugged, a mode change re-centers it). */
static bool
constraint_cursor_position(struct cg_seat *seat, struct wlr_pointer_constraint_v1 *constraint, double *sx, double *sy)
{
	struct wlr_scene_node *node = scene_node_for_surface(&seat->server->scene->tree.node, constraint->surface);
	int lx, ly;
	if (!node || !wlr_scene_node_coords(node, &lx, &ly)) {
		return false;
	}
	*sx = seat->cursor->x - lx;
	*sy = seat->cursor->y - ly;
	return true;
}

/* On deactivation, warp the cursor to the client's cursor-position hint so it
 * reappears where the client last drew its own cursor. The hint is in surface
 * coordinates. */
static void
warp_to_constraint_cursor_hint(struct cg_seat *seat)
{
	struct wlr_pointer_constraint_v1 *constraint = seat->active_constraint;

	/* Only a constraint that was in effect has a cursor to restore. */
	if (!seat->constraint_enforced || !constraint->current.cursor_hint.enabled) {
		return;
	}

	double cursor_sx, cursor_sy;
	if (!constraint_cursor_position(seat, constraint, &cursor_sx, &cursor_sy)) {
		return;
	}
	double sx = constraint->current.cursor_hint.x;
	double sy = constraint->current.cursor_hint.y;
	if (!wlr_cursor_warp(seat->cursor, NULL, seat->cursor->x + sx - cursor_sx, seat->cursor->y + sy - cursor_sy)) {
		/* The hint maps outside the output layout: the cursor did not
		 * move, so leave the surface-local position alone too. */
		return;
	}
	if (seat->seat->pointer_state.focused_surface == constraint->surface) {
		/* Fix up the seat's surface-local position without sending a motion event. */
		wlr_seat_pointer_warp(seat->seat, sx, sy);
	}
}

/* The region a constraint confines the pointer to: its own region intersected
 * with the surface's input region. wlroots computes this into
 * constraint->region, but only on a commit of the surface — and at creation
 * only for a constraint created with a region — so a constraint created
 * without one has an empty region there until its next commit. Recompute it
 * the same way instead of relying on, or writing to, wlroots' copy: writing to
 * it would break the change detection that emits the set_region signal. */
static void
constraint_effective_region(struct wlr_pointer_constraint_v1 *constraint, pixman_region32_t *region)
{
	if (pixman_region32_not_empty(&constraint->current.region)) {
		pixman_region32_intersect(region, &constraint->surface->input_region, &constraint->current.region);
	} else {
		pixman_region32_copy(region, &constraint->surface->input_region);
	}
}

/* The point of the region nearest to (sx, sy), half a pixel inside the
 * region's integer extents so the result still passes a floored
 * pixman_region32_contains_point() test. Returns false for an empty region. */
static bool
region_nearest_point(pixman_region32_t *region, double sx, double sy, double *nx, double *ny)
{
	int nboxes;
	pixman_box32_t *boxes = pixman_region32_rectangles(region, &nboxes);
	double best = INFINITY;
	for (int i = 0; i < nboxes; i++) {
		double x = fmax(boxes[i].x1 + 0.5, fmin(sx, boxes[i].x2 - 0.5));
		double y = fmax(boxes[i].y1 + 0.5, fmin(sy, boxes[i].y2 - 0.5));
		double distance = (x - sx) * (x - sx) + (y - sy) * (y - sy);
		if (distance < best) {
			best = distance;
			*nx = x;
			*ny = y;
		}
	}
	return nboxes > 0;
}

/* Both a locked and a confined pointer must be inside the constraint region
 * while the constraint is in effect; warp the cursor to the nearest point
 * inside the region if it is outside. sx/sy are the cursor's surface-local
 * coordinates and are updated on warp. Returns false if the cursor could not
 * be placed inside the region — it is empty, or the point nearest to the
 * cursor lies outside the output layout — in which case the cursor did not
 * move. */
static bool
constraint_warp_into_region(struct cg_seat *seat, pixman_region32_t *region, double *sx, double *sy)
{
	if (pixman_region32_contains_point(region, (int) floor(*sx), (int) floor(*sy), NULL)) {
		return true;
	}

	double nx = 0, ny = 0;
	if (!region_nearest_point(region, *sx, *sy, &nx, &ny)) {
		/* An empty region has no inside to warp to. */
		return false;
	}
	if (!wlr_cursor_warp(seat->cursor, NULL, seat->cursor->x + nx - *sx, seat->cursor->y + ny - *sy)) {
		return false;
	}
	*sx = nx;
	*sy = ny;
	return true;
}

/* Put the cursor inside the active constraint's effective region and keep the
 * seat's surface-local position in sync with it. Returns false if the region
 * cannot hold the cursor, in which case the constraint must not be enforced:
 * a pointer outside the region cannot be confined by it, and every
 * wlr_region_confine() from there would fail. */
static bool
constraint_enforce_region(struct cg_seat *seat, bool send_motion, uint32_t time_msec)
{
	struct wlr_pointer_constraint_v1 *constraint = seat->active_constraint;

	double sx, sy;
	if (!constraint_cursor_position(seat, constraint, &sx, &sy)) {
		return false;
	}

	if (!constraint_warp_into_region(seat, &seat->constraint_region, &sx, &sy)) {
		return false;
	}
	/* Compare against the seat's surface-local position, not the cursor's
	 * layout position: when the surface moved under a stationary cursor,
	 * no warp happens but sx/sy still changed, and a stale surface-local
	 * position would misdirect every clamp in constrain_delta(). */
	struct wlr_seat_pointer_state *pointer_state = &seat->seat->pointer_state;
	if (pointer_state->focused_surface != constraint->surface ||
	    (pointer_state->sx == sx && pointer_state->sy == sy)) {
		return true;
	}
	if (send_motion) {
		/* zwp_confined_pointer_v1.set_region: "If warped, a
		 * wl_pointer.motion event will be emitted." Sending the motion
		 * updates the seat's surface-local position too — warping that
		 * silently first would make the motion look like a duplicate
		 * and drop it, leaving the client uncorrected. */
		wlr_seat_pointer_notify_motion(seat->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_warp(seat->seat, sx, sy);
	}
	return true;
}

static void
handle_constraint_sync_idle(void *data)
{
	struct cg_seat *seat = data;
	seat->constraint_sync_idle = NULL;

	if (!seat->active_constraint || seat->constraint_enforced == seat->constraint_notified) {
		return;
	}
	seat->constraint_notified = seat->constraint_enforced;
	if (seat->constraint_notified) {
		wlr_pointer_constraint_v1_send_activated(seat->active_constraint);
	} else {
		/* This destroys a oneshot constraint, which runs
		 * handle_constraint_destroy() and detaches us. */
		wlr_pointer_constraint_v1_send_deactivated(seat->active_constraint);
	}
}

/* Tell the client whether its constraint is in effect, from an idle callback.
 * Deactivating a oneshot constraint destroys it, and a region update that can
 * cause that is emitted from inside the surface commit whose synced state
 * wlroots is still iterating over — freeing the constraint there would pull an
 * entry out from under that loop. The idle runs before the client is flushed,
 * so it sees no delay, and a burst of region updates collapses into at most
 * one event. */
static void
constraint_sync_client(struct cg_seat *seat)
{
	if (seat->constraint_sync_idle || seat->constraint_enforced == seat->constraint_notified) {
		return;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(seat->server->wl_display);
	seat->constraint_sync_idle = wl_event_loop_add_idle(loop, handle_constraint_sync_idle, seat);
	if (!seat->constraint_sync_idle) {
		wlr_log(WLR_ERROR, "Cannot notify a client of its pointer constraint's state");
	}
}

/* Drop all of our state for the active constraint. An inactive constraint has
 * none, so the listeners live in cg_seat rather than in a per-constraint
 * allocation and are only linked while a constraint is active. */
static void
constraint_detach(struct cg_seat *seat)
{
	seat->active_constraint = NULL;
	seat->constraint_enforced = false;
	seat->constraint_notified = false;
	pixman_region32_clear(&seat->constraint_region);
	if (seat->constraint_sync_idle) {
		wl_event_source_remove(seat->constraint_sync_idle);
		seat->constraint_sync_idle = NULL;
	}
	wl_list_remove(&seat->constraint_set_region.link);
	wl_list_init(&seat->constraint_set_region.link);
	wl_list_remove(&seat->constraint_destroy.link);
	wl_list_init(&seat->constraint_destroy.link);
}

/* Re-evaluate whether the active constraint can be enforced from the cursor's
 * and the surface's current positions, and tell the client when the answer
 * changes. send_motion asks for the wl_pointer.motion event that
 * zwp_confined_pointer_v1.set_region mandates for a warp it causes. */
static void
constraint_update(struct cg_seat *seat, bool send_motion, uint32_t time_msec)
{
	struct wlr_pointer_constraint_v1 *constraint = seat->active_constraint;
	if (!constraint) {
		return;
	}

	if (seat->constraint_enforced && constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		/* The lock region only gates activation — the protocol calls it
		 * "where the pointer must be in order for the lock to
		 * activate" — so a pointer that is already locked stays pinned
		 * whatever happens to the region. */
		return;
	}

	send_motion = send_motion && seat->constraint_enforced;
	seat->constraint_enforced = constraint_enforce_region(seat, send_motion, time_msec);
	constraint_sync_client(seat);
}

static uint32_t
now_msec(void)
{
	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t) (now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* wlroots recomputed the constraint's effective region: a committed set_region
 * request or a changed input region altered it. Re-validate the cursor
 * position so a confined pointer cannot end up — and then be stuck — outside
 * the new region. */
static void
handle_constraint_set_region(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, constraint_set_region);

	constraint_effective_region(seat->active_constraint, &seat->constraint_region);
	constraint_update(seat, true, now_msec());
}

static void
handle_constraint_destroy(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, constraint_destroy);

	/* The resource is going away: apply the hint, but send nothing. */
	warp_to_constraint_cursor_hint(seat);
	constraint_detach(seat);
}

static void
constraint_set_active(struct cg_seat *seat, struct wlr_pointer_constraint_v1 *constraint)
{
	if (seat->active_constraint == constraint) {
		return;
	}

	if (seat->active_constraint) {
		struct wlr_pointer_constraint_v1 *prev = seat->active_constraint;
		bool notified = seat->constraint_notified;
		warp_to_constraint_cursor_hint(seat);
		/* send_deactivated() destroys a oneshot constraint
		 * synchronously; detach our state first so the destroy signal
		 * finds no listener of ours left. */
		constraint_detach(seat);
		if (notified) {
			wlr_pointer_constraint_v1_send_deactivated(prev);
		}
	}

	if (!constraint) {
		return;
	}

	seat->active_constraint = constraint;
	seat->constraint_set_region.notify = handle_constraint_set_region;
	wl_signal_add(&constraint->events.set_region, &seat->constraint_set_region);
	seat->constraint_destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &seat->constraint_destroy);

	constraint_effective_region(constraint, &seat->constraint_region);
	constraint_update(seat, false, 0);
}

void
handle_pointer_constraint(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct cg_seat *seat = server->seat;

	if (seat->seat->pointer_state.focused_surface == constraint->surface) {
		constraint_set_active(seat, constraint);
	}
}

/* The views moved — an output was hotplugged, a mode or scale change
 * re-centered them — and wlroots warped the cursor to keep it inside the
 * layout. Both the constrained surface and the cursor may have moved, so the
 * constraint has to be enforced again from their new positions: a confined
 * pointer left outside its region would otherwise stay wedged there for as
 * long as the constraint lives. */
void
seat_revalidate_pointer_constraint(struct cg_seat *seat)
{
	if (!seat || !seat->active_constraint) {
		return;
	}

	constraint_effective_region(seat->active_constraint, &seat->constraint_region);
	constraint_update(seat, true, now_msec());
}

static void
handle_pointer_focus_change(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;

	/* Constraints follow pointer focus. */
	struct wlr_pointer_constraint_v1 *constraint = NULL;
	if (event->new_surface) {
		constraint = wlr_pointer_constraints_v1_constraint_for_surface(seat->server->pointer_constraints,
									       event->new_surface, seat->seat);
	}
	constraint_set_active(seat, constraint);
}

/* Whether the active constraint pins the cursor in place. */
static bool
constraint_locks_cursor(struct cg_seat *seat)
{
	return seat->constraint_enforced && seat->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

/* Clamp a cursor motion to what the active constraint allows. dx/dy are in
 * layout coordinates, which have the same scale as the constrained surface's
 * coordinates. */
static void
constrain_delta(struct cg_seat *seat, double *dx, double *dy)
{
	if (!seat->constraint_enforced) {
		return;
	}
	if (constraint_locks_cursor(seat)) {
		/* Locked: the cursor stays pinned; the client follows the
		 * relative stream sent by process_cursor_motion(). */
		*dx = *dy = 0;
		return;
	}

	/* Confined: clamp the target to the constraint region, sliding along
	 * its edges. The seat's surface-local position is the constrained
	 * surface's — the active constraint always belongs to the focused
	 * surface — and every warp of ours keeps it in sync with the cursor. */
	struct wlr_seat_pointer_state *pointer_state = &seat->seat->pointer_state;
	double sx = pointer_state->sx;
	double sy = pointer_state->sy;
	double sx_confined, sy_confined;
	if (wlr_region_confine(&seat->constraint_region, sx, sy, sx + *dx, sy + *dy, &sx_confined, &sy_confined)) {
		*dx = sx_confined - sx;
		*dy = sy_confined - sy;
		return;
	}

	/* wlr_region_confine() cannot clamp from a start point the region does
	 * not contain, and the cursor can be left outside it after all: the
	 * surface moved out from under it, or the region shrank away from it
	 * between two of our checks. Head back to the nearest point inside
	 * rather than freezing until the constraint dies. */
	double nx = 0, ny = 0;
	if (region_nearest_point(&seat->constraint_region, sx, sy, &nx, &ny)) {
		*dx = nx - sx;
		*dy = ny - sy;
	} else {
		*dx = *dy = 0;
	}
}

/* Bring the pointer focus, and the client's view of the pointer, in line with
 * the cursor. scene_changed tells apart the two ways this is reached: a cursor
 * motion, or a change in what is on screen with the cursor standing still. */
static void
process_cursor_motion(struct cg_seat *seat, uint32_t time_msec, double dx, double dy, double dx_unaccel,
		      double dy_unaccel, bool scene_changed)
{
	struct wlr_seat *wlr_seat = seat->seat;

	/* Send the relative motion before any focus change below: a surface
	 * that is entered — and whose constraint is activated — by this very
	 * motion must not receive the traversal delta as its first event. */
	if (dx != 0 || dy != 0) {
		wlr_relative_pointer_manager_v1_send_relative_motion(seat->server->relative_pointer_manager, wlr_seat,
								     (uint64_t) time_msec * 1000, dx, dy, dx_unaccel,
								     dy_unaccel);
	}

	if (!scene_changed && constraint_locks_cursor(seat)) {
		/* The cursor is pinned — constrain_delta() zeroed the motion —
		 * so nothing under it changed, the focus cannot move, and a
		 * locked pointer receives no wl_pointer.motion anyway. The
		 * relative stream above is all the client gets. */
		wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct cg_view *view = desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy);

	/* Entering the surface activates its constraint (see
	 * handle_pointer_focus_change()); warp the pointer into the constraint
	 * region first, so the client is never told of an out-of-region
	 * position — a lock in particular guarantees the pointer is inside the
	 * region when it activates. The warp moves the cursor, so look up what
	 * is topmost again: the constrained surface may well be occluded at the
	 * position we warped to. */
	if (view && surface != wlr_seat->pointer_state.focused_surface) {
		struct wlr_pointer_constraint_v1 *next = wlr_pointer_constraints_v1_constraint_for_surface(
			seat->server->pointer_constraints, surface, wlr_seat);
		if (next) {
			pixman_region32_t region;
			pixman_region32_init(&region);
			constraint_effective_region(next, &region);
			double cursor_x = seat->cursor->x;
			double cursor_y = seat->cursor->y;
			constraint_warp_into_region(seat, &region, &sx, &sy);
			pixman_region32_fini(&region);
			if (seat->cursor->x != cursor_x || seat->cursor->y != cursor_y) {
				view = desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx,
						       &sy);
			}
		}
	}

	if (!view) {
		wlr_seat_pointer_clear_focus(wlr_seat);
	} else if (surface != wlr_seat->pointer_state.focused_surface) {
		double pre_enter_x = seat->cursor->x;
		double pre_enter_y = seat->cursor->y;
		wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);
		if (seat->cursor->x != pre_enter_x || seat->cursor->y != pre_enter_y) {
			/* Entering the surface deactivated the previous
			 * constraint, whose cursor-position hint warped the
			 * cursor; rebase on the warped position so the motion
			 * below does not undo the warp. */
			view = desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy);
			if (!view) {
				wlr_seat_pointer_clear_focus(wlr_seat);
			} else if (surface != wlr_seat->pointer_state.focused_surface) {
				wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);
			}
		}
	}
	if (view) {
		/* A locked pointer receives no motion events at all — not even
		 * if the surface moved under the pinned cursor and sx/sy
		 * changed. Read the constraint after the enter: entering the
		 * surface may have just activated it. */
		if (constraint_locks_cursor(seat)) {
			/* Keep the seat's surface-local position in sync all the
			 * same. An enter for the already-focused surface is a
			 * no-op in wlroots, so a warp between the two enters
			 * above would leave it at the pre-warp coordinates — and
			 * every later motion would be deduplicated against
			 * them. */
			wlr_seat_pointer_warp(wlr_seat, sx, sy);
		} else {
			wlr_seat_pointer_notify_motion(wlr_seat, time_msec, sx, sy);
		}
	}

	struct cg_drag_icon *drag_icon;
	wl_list_for_each (drag_icon, &seat->drag_icons, link) {
		drag_icon_update_position(drag_icon);
	}

	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

	/* The synthesized relative motion is the distance the device travelled,
	 * which is only the distance to the cursor as long as the cursor is
	 * free to follow it: a constraint can hold the cursor back. Difference
	 * against this device's own previous absolute position instead, and
	 * fall back to the cursor until it has one. */
	struct cg_pointer *pointer = pointer_from_wlr_pointer(seat, event->pointer);
	double dx, dy;
	if (pointer && pointer->last_abs_valid) {
		dx = lx - pointer->last_abs_x;
		dy = ly - pointer->last_abs_y;
	} else if (!seat->constraint_enforced) {
		dx = lx - seat->cursor->x;
		dy = ly - seat->cursor->y;
	} else {
		/* Nothing meaningful to synthesize: the device has no history,
		 * and the cursor is not where the device is. */
		dx = dy = 0;
	}
	if (pointer) {
		pointer->last_abs_x = lx;
		pointer->last_abs_y = ly;
		pointer->last_abs_valid = true;
	}

	if (seat->constraint_enforced) {
		/* Turn the absolute target into a motion the constraint can
		 * clamp, then warp to the closest point the layout and the
		 * device's output mapping allow: a target inside the constraint
		 * region can still fall outside those, and dropping the event
		 * instead would wedge the pointer. */
		double move_x = lx - seat->cursor->x;
		double move_y = ly - seat->cursor->y;
		constrain_delta(seat, &move_x, &move_y);
		if (move_x != 0 || move_y != 0) {
			wlr_cursor_warp_closest(seat->cursor, &event->pointer->base, seat->cursor->x + move_x,
						seat->cursor->y + move_y);
		}
	} else {
		wlr_cursor_warp_absolute(seat->cursor, &event->pointer->base, event->x, event->y);
	}

	process_cursor_motion(seat, event->time_msec, dx, dy, dx, dy, false);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
handle_cursor_motion_relative(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_relative);
	struct wlr_pointer_motion_event *event = data;

	/* Constrain the motion the cursor makes, but pass the raw deltas on to
	 * process_cursor_motion() so the relative stream stays unclamped. */
	double dx = event->delta_x;
	double dy = event->delta_y;
	constrain_delta(seat, &dx, &dy);
	if (dx != 0 || dy != 0) {
		wlr_cursor_move(seat->cursor, &event->pointer->base, dx, dy);
	}

	process_cursor_motion(seat, event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx,
			      event->unaccel_dy, false);
	wlr_idle_notifier_v1_notify_activity(seat->server->idle, seat->seat);
}

static void
drag_icon_update_position(struct cg_drag_icon *drag_icon)
{
	struct wlr_drag_icon *wlr_icon = drag_icon->wlr_drag_icon;
	struct cg_seat *seat = drag_icon->seat;
	struct wlr_touch_point *point;

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

	wlr_scene_node_set_position(&drag_icon->scene_tree->node, drag_icon->lx, drag_icon->ly);
}

static void
handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
	struct cg_drag_icon *drag_icon = wl_container_of(listener, drag_icon, destroy);

	wl_list_remove(&drag_icon->link);
	wl_list_remove(&drag_icon->destroy.link);
	wlr_scene_node_destroy(&drag_icon->scene_tree->node);
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
	drag_icon->scene_tree = wlr_scene_subsurface_tree_create(&seat->server->scene->tree, wlr_drag_icon->surface);
	if (!drag_icon->scene_tree) {
		free(drag_icon);
		return;
	}

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
	wl_list_remove(&seat->cursor_motion_relative.link);
	wl_list_remove(&seat->cursor_motion_absolute.link);
	wl_list_remove(&seat->cursor_button.link);
	wl_list_remove(&seat->cursor_axis.link);
	wl_list_remove(&seat->cursor_frame.link);
	wl_list_remove(&seat->pointer_focus_change.link);
	/* wlroots destroys any surviving constraint after us (its seat destroy
	 * listeners were added later); unlink so its destroy signal does not
	 * run our handler on the freed seat. */
	wl_list_remove(&seat->constraint_set_region.link);
	wl_list_remove(&seat->constraint_destroy.link);
	if (seat->constraint_sync_idle) {
		wl_event_source_remove(seat->constraint_sync_idle);
	}
	pixman_region32_fini(&seat->constraint_region);
	wl_list_remove(&seat->touch_down.link);
	wl_list_remove(&seat->touch_up.link);
	wl_list_remove(&seat->touch_motion.link);
	wl_list_remove(&seat->touch_frame.link);
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

	seat->seat = wlr_seat_create(server->wl_display, "default");
	if (!seat->seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat");
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

	seat->cursor_motion_relative.notify = handle_cursor_motion_relative;
	wl_signal_add(&seat->cursor->events.motion, &seat->cursor_motion_relative);
	seat->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&seat->cursor->events.motion_absolute, &seat->cursor_motion_absolute);
	seat->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&seat->cursor->events.button, &seat->cursor_button);
	seat->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&seat->cursor->events.axis, &seat->cursor_axis);
	seat->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&seat->cursor->events.frame, &seat->cursor_frame);

	seat->pointer_focus_change.notify = handle_pointer_focus_change;
	wl_signal_add(&seat->seat->pointer_state.events.focus_change, &seat->pointer_focus_change);
	/* Only linked while a constraint is active; keep them initialized so
	 * removal is always safe. */
	wl_list_init(&seat->constraint_set_region.link);
	wl_list_init(&seat->constraint_destroy.link);
	pixman_region32_init(&seat->constraint_region);

	seat->touch_down.notify = handle_touch_down;
	wl_signal_add(&seat->cursor->events.touch_down, &seat->touch_down);
	seat->touch_up.notify = handle_touch_up;
	wl_signal_add(&seat->cursor->events.touch_up, &seat->touch_up);
	seat->touch_motion.notify = handle_touch_motion;
	wl_signal_add(&seat->cursor->events.touch_motion, &seat->touch_motion);
	seat->touch_frame.notify = handle_touch_frame;
	wl_signal_add(&seat->cursor->events.touch_frame, &seat->touch_frame);

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

	server->new_virtual_keyboard.notify = handle_virtual_keyboard;
	server->new_virtual_pointer.notify = handle_virtual_pointer;

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

	struct cg_keyboard_group *keyboard_group, *keyboard_group_tmp;
	wl_list_for_each_safe (keyboard_group, keyboard_group_tmp, &seat->keyboard_groups, link) {
		keyboard_group_destroy(keyboard_group);
	}

	// Destroying the wlr seat will trigger the destroy handler on our seat,
	// which will in turn free it.
	wlr_seat_destroy(seat->seat);
}

struct cg_view *
seat_get_focus(struct cg_seat *seat)
{
	struct wlr_surface *prev_surface = seat->seat->keyboard_state.focused_surface;
	if (!prev_surface) {
		return NULL;
	}
	return view_from_wlr_surface(prev_surface);
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
		if (!wlr_xwayland_surface_override_redirect_wants_focus(xwayland_view->xwayland_surface)) {
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

	/* The newly focused view was raised over the cursor: re-resolve what the
	 * cursor is over, even while a lock pins it in place. */
	process_cursor_motion(seat, -1, 0, 0, 0, 0, true);
}

void
seat_center_cursor(struct cg_seat *seat)
{
	/* Place the cursor in the center of the output layout. */
	struct wlr_box layout_box;
	wlr_output_layout_get_box(seat->server->output_layout, NULL, &layout_box);
	wlr_cursor_warp(seat->cursor, NULL, layout_box.width / 2, layout_box.height / 2);
}
