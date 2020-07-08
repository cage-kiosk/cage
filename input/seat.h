#ifndef CG_SEAT_H
#define CG_SEAT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

#include "cursor.h"

struct cg_seat {
	struct wlr_seat *wlr_seat;
	struct wl_listener seat_destroy;

	struct cg_cursor *cursor;

	struct wl_list pointers; // cg_pointer::link
};

void cage_seat_init(struct cg_seat *seat, struct wlr_seat *wlr_seat, struct cg_cursor *cursor);
void cage_seat_fini(struct cg_seat *seat);

#endif
