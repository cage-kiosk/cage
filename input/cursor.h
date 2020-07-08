#ifndef CG_CURSOR_H
#define CG_CURSOR_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#define DEFAULT_XCURSOR "left_ptr"
#define XCURSOR_SIZE 24

struct cg_cursor {
	struct wlr_cursor *wlr_cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wlr_seat *wlr_seat;

	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wl_listener request_set_cursor;
	struct wl_listener surface_destroy;

	struct {
		/**
		 * Proxy wlr_cursor's motion and motion_absolute signals to the compositor.
		 * Note that cg_cursor has already taken care of calling the respective
		 * wlr_cursor functions; only the compositor-specific handling such as
		 * focus changing need to be implemented. This signal is emitted for both
		 * relative and absolute cursor motion.
		 */
		struct wl_signal motion;

		/**
		 * Proxy wlr_cursor's button signal to the compositor. Note that cg_cursor
		 * has already taken care of calling the respective wlr_cursor functions;
		 * only the compositor-specific handling such as focus changing needs to
		 * be implemented.
		 */
		struct wl_signal button;
	} events;
};

void cage_cursor_set_image(struct cg_cursor *cursor, const char *path);
void cage_cursor_init(struct cg_cursor *cursor, struct wlr_cursor *wlr_cursor,
		      struct wlr_xcursor_manager *xcursor_manager, struct wlr_seat *wlr_seat);
void cage_cursor_fini(struct cg_cursor *cursor);

#endif
