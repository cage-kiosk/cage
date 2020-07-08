/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "keyboard_group.h"

static void
handle_keyboard_group_key(struct wl_listener *listener, void *user_data)
{
	struct cg_keyboard_group *group = wl_container_of(listener, group, key);
	struct wlr_input_device *wlr_input_device = group->wlr_keyboard_group->input_device;
	struct wlr_event_keyboard_key *event = user_data;

	wlr_seat_set_keyboard(group->wlr_seat, wlr_input_device);
	wlr_seat_keyboard_notify_key(group->wlr_seat, event->time_msec, event->keycode, event->state);

	wlr_seat_set_keyboard(group->wlr_seat, wlr_input_device);
	wlr_seat_keyboard_notify_key(group->wlr_seat, event->time_msec, event->keycode, event->state);
}

static void
handle_keyboard_group_modifiers(struct wl_listener *listener, void *user_data)
{
	struct cg_keyboard_group *group = wl_container_of(listener, group, modifiers);
	struct wlr_input_device *wlr_input_device = group->wlr_keyboard_group->input_device;

	wlr_seat_set_keyboard(group->wlr_seat, wlr_input_device);
	wlr_seat_keyboard_notify_modifiers(group->wlr_seat, &wlr_input_device->keyboard->modifiers);

	// wlr_idle_notify_activity(seat->server->idle, seat->seat);
}

bool
cage_keyboard_group_add(struct cg_keyboard_group *group, struct wlr_input_device *wlr_input_device)
{
	struct wlr_keyboard_group *wlr_group = group->wlr_keyboard_group;
	bool added = wlr_keyboard_group_add_keyboard(wlr_group, wlr_input_device->keyboard);
	if (added) {
		wlr_seat_set_keyboard(group->wlr_seat, wlr_input_device);
	}
	return added;
}

bool
cage_keyboard_group_init(struct cg_keyboard_group *group, struct wlr_input_device *device, struct wlr_seat *wlr_seat)
{
	assert(group != NULL);
	assert(device != NULL);
	assert(wlr_seat != NULL);

	struct wlr_keyboard_group *wlr_keyboard_group = NULL;
	struct wlr_keyboard *wlr_keyboard = device->keyboard;

	wlr_keyboard_group = wlr_keyboard_group_create();
	if (!wlr_keyboard_group) {
		wlr_log(WLR_ERROR, "Failed to create wlr keyboard group.");
		return false;
	}
	wlr_keyboard_group->data = group;
	group->wlr_keyboard_group = wlr_keyboard_group;
	group->wlr_seat = wlr_seat;

	wlr_keyboard_set_keymap(&wlr_keyboard_group->keyboard, device->keyboard->keymap);
	wlr_keyboard_set_repeat_info(&wlr_keyboard_group->keyboard, wlr_keyboard->repeat_info.rate,
				     wlr_keyboard->repeat_info.delay);
	wlr_keyboard_group_add_keyboard(wlr_keyboard_group, wlr_keyboard);

	wl_signal_add(&wlr_keyboard_group->keyboard.events.key, &group->key);
	group->key.notify = handle_keyboard_group_key;
	wl_signal_add(&wlr_keyboard_group->keyboard.events.modifiers, &group->modifiers);
	group->modifiers.notify = handle_keyboard_group_modifiers;

	wlr_seat_set_keyboard(wlr_seat, device);
	return true;
}

void
cage_keyboard_group_fini(struct cg_keyboard_group *group)
{
	assert(group != NULL);

	group->wlr_seat = NULL;
	if (group->wlr_keyboard_group) {
		wlr_keyboard_group_destroy(group->wlr_keyboard_group);
	}

	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);

	free(group);
}
