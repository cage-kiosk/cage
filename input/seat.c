/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "cursor.h"
#include "keyboard_group.h"
#include "pointer.h"
#include "seat.h"

struct wlr_surface *
cage_seat_get_focus(struct cg_seat *seat)
{
	assert(seat != NULL);

	return seat->wlr_seat->keyboard_state.focused_surface;
}

void
cage_seat_add_new_keyboard(struct cg_seat *seat, struct wlr_input_device *device)
{
	assert(seat != NULL);
	assert(device != NULL);

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

	struct cg_keyboard_group *group;
	wl_list_for_each (group, &seat->keyboard_groups, link) {
		if (cage_keyboard_group_add(group, device)) {
			wlr_log(WLR_DEBUG, "Added new keyboard to existing group");
			return;
		}
	}

	wlr_log(WLR_DEBUG, "Created new keyboard group");

	/* This is reached if and only if the keyboard could not be inserted into
	 * any group. */
	struct cg_keyboard_group *keyboard_group = calloc(1, sizeof(struct cg_keyboard_group));
	if (!keyboard_group) {
		wlr_log(WLR_ERROR, "Cannot allocate keyboard group");
		return;
	}

	if (!cage_keyboard_group_init(keyboard_group, device, seat->wlr_seat)) {
		wlr_log(WLR_ERROR, "Failed setting up keyboard group");
		cage_keyboard_group_fini(keyboard_group);
		return;
	}

	wl_list_insert(&seat->keyboard_groups, &keyboard_group->link);
}

void
cage_seat_add_new_pointer(struct cg_seat *seat, struct wlr_input_device *device)
{
	struct cg_pointer *pointer = calloc(1, sizeof(struct cg_pointer));
	if (!pointer) {
		wlr_log(WLR_ERROR, "Cannot allocate pointer");
		return;
	}

	cage_pointer_init(pointer, device);
	wlr_cursor_attach_input_device(seat->cursor->wlr_cursor, device);
	wl_list_insert(&seat->pointers, &pointer->link);
}

void
cage_seat_update_capabilities(struct cg_seat *seat)
{
	uint32_t caps = 0;

	if (!wl_list_empty(&seat->pointers)) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	if (!wl_list_empty(&seat->keyboard_groups)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(seat->wlr_seat, caps);

	/* Hide cursor if the seat doesn't have pointer capability. */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
		cage_cursor_set_image(seat->cursor, NULL);
	} else {
		cage_cursor_set_image(seat->cursor, DEFAULT_XCURSOR);
	}
}

static void
handle_seat_destroy(struct wl_listener *listener, void *user_data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, seat_destroy);
	cage_seat_fini(seat);
}

void
cage_seat_init(struct cg_seat *seat, struct wlr_seat *wlr_seat, struct cg_cursor *cursor)
{
	assert(seat != NULL);
	assert(wlr_seat != NULL);
	assert(cursor != NULL);

	seat->wlr_seat = wlr_seat;
	seat->cursor = cursor;

	seat->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->wlr_seat->events.destroy, &seat->seat_destroy);

	wl_list_init(&seat->pointers);
	wl_list_init(&seat->keyboard_groups);
}

void
cage_seat_fini(struct cg_seat *seat)
{
	if (!seat) {
		return;
	}

	wlr_seat_destroy(seat->wlr_seat);
	wl_list_remove(&seat->seat_destroy.link);

	cage_cursor_fini(seat->cursor);

	struct cg_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe (pointer, pointer_tmp, &seat->pointers, link) {
		cage_pointer_fini(pointer);
	}

	struct cg_keyboard_group *keyboard_group, *keyboard_group_tmp;
	wl_list_for_each_safe (keyboard_group, keyboard_group_tmp, &seat->keyboard_groups, link) {
		cage_keyboard_group_fini(keyboard_group);
	}

	free(seat);
}
