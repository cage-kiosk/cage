#ifndef CG_SEAT_H
#define CG_SEAT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>

#include "cursor.h"

struct cg_seat {
	struct wlr_seat *wlr_seat;
	struct wl_listener seat_destroy;

	struct cg_cursor *cursor;

	struct wl_list pointers; // cg_pointer::link
	struct wl_list keyboard_groups; // cg_keyboard_group::link
};

struct wlr_surface *cage_seat_get_focus(struct cg_seat *seat);
void cage_seat_add_new_keyboard(struct cg_seat *seat, struct wlr_input_device *device);
void cage_seat_add_new_pointer(struct cg_seat *seat, struct wlr_input_device *device);
void cage_seat_update_capabilities(struct cg_seat *seat);
void cage_seat_init(struct cg_seat *seat, struct wlr_seat *wlr_seat, struct cg_cursor *cursor);
void cage_seat_fini(struct cg_seat *seat);

#endif
