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
#include <wlr/types/wlr_seat.h>

#include "cursor.h"
#include "seat.h"

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

	free(seat);
}
