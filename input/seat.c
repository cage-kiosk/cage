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
#include "pointer.h"
#include "seat.h"

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

	free(seat);
}
