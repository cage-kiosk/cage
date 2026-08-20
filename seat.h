#ifndef CG_SEAT_H
#define CG_SEAT_H

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>

#include "server.h"
#include "view.h"

#define DEFAULT_XCURSOR "left_ptr"
#define XCURSOR_SIZE 24

struct cg_seat {
	struct wlr_seat *seat;
	struct cg_server *server;
	struct wl_listener destroy;

	struct wl_list keyboards;
	struct wl_list keyboard_groups;
	struct wl_list pointers;
	struct wl_list touch;
	struct wl_listener new_input;

	struct wlr_cursor *cursor;
	struct wl_listener cursor_motion_relative;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	/* The constraint of the pointer-focused surface, if it has one. It is
	 * only enforced — and only reported to the client as active — while its
	 * effective region can hold the cursor. */
	struct wlr_pointer_constraint_v1 *active_constraint;
	/* The effective region of the active constraint (its region intersected
	 * with the surface's input region), in surface coordinates. Ours to
	 * compute: constraint->region is wlroots' own state. */
	pixman_region32_t constraint_region;
	bool constraint_enforced;
	/* Whether the client has been sent an activated event that no
	 * deactivated event has followed yet. It trails constraint_enforced
	 * until constraint_sync_idle runs. */
	bool constraint_notified;
	struct wl_event_source *constraint_sync_idle;
	struct wl_listener constraint_set_region;
	struct wl_listener constraint_destroy;
	struct wl_listener pointer_focus_change;

	int32_t touch_id;
	double touch_lx;
	double touch_ly;
	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;
	struct wl_listener touch_frame;

	struct wl_list drag_icons;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;

	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
};

struct cg_keyboard_group {
	struct wlr_keyboard_group *wlr_group;
	struct cg_seat *seat;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_list link; // cg_seat::keyboard_groups
	bool is_virtual;
};

struct cg_pointer {
	struct wl_list link; // seat::pointers
	struct cg_seat *seat;
	struct wlr_pointer *pointer;

	/* Reference for synthesizing relative motion from this device's
	 * absolute events: a constraint can keep the cursor from following the
	 * device, so consecutive absolute positions must be differenced
	 * directly instead of against the cursor. The reference is per device:
	 * differencing positions of two different absolute devices would turn
	 * their distance into a spurious delta. */
	double last_abs_x, last_abs_y;
	bool last_abs_valid;

	struct wl_listener destroy;
};

struct cg_touch {
	struct wl_list link; // seat::touch
	struct cg_seat *seat;
	struct wlr_touch *touch;

	struct wl_listener destroy;
};

struct cg_drag_icon {
	struct wl_list link; // seat::drag_icons
	struct cg_seat *seat;
	struct wlr_drag_icon *wlr_drag_icon;
	struct wlr_scene_tree *scene_tree;

	/* The drag icon has a position in layout coordinates. */
	double lx, ly;

	struct wl_listener destroy;
};

struct cg_seat *seat_create(struct cg_server *server, struct wlr_backend *backend);
void seat_destroy(struct cg_seat *seat);
struct cg_view *seat_get_focus(struct cg_seat *seat);
void seat_set_focus(struct cg_seat *seat, struct cg_view *view);
void seat_center_cursor(struct cg_seat *seat);
void seat_revalidate_pointer_constraint(struct cg_seat *seat);

void handle_request_set_shape(struct wl_listener *listener, void *data);
void handle_pointer_constraint(struct wl_listener *listener, void *data);
#endif
