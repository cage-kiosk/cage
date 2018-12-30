#ifndef CG_SEAT_H
#define CG_SEAT_H

#include <wayland-server.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "server.h"
#include "view.h"

#define DEFAULT_XCURSOR "left_ptr"

struct cg_seat {
	struct wlr_seat *seat;
	struct cg_server *server;
	struct wl_listener destroy;

	struct wl_list keyboards;
	struct wl_list pointers;
	struct wl_listener new_input;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener request_set_cursor;
};

struct cg_keyboard {
	struct wl_list link; // seat::keyboards
	struct cg_seat *seat;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct cg_pointer {
	struct wl_list link; // seat::pointers
	struct cg_seat *seat;
	struct wlr_input_device *device;

	struct wl_listener destroy;
};

struct cg_seat *cg_seat_create(struct cg_server *server);
void cg_seat_destroy(struct cg_seat *seat);
struct cg_view *seat_get_focus(struct cg_seat *seat);
void seat_set_focus(struct cg_seat *seat, struct cg_view *view);

#endif
