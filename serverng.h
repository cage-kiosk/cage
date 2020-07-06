#ifndef CG_SERVER_H
#define CG_SERVER_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>

#include "desktop/output.h"
#include "input/seat.h"

struct cg_server {
	struct wl_display *wl_display;

	struct cg_seat *seat;
	struct wl_listener new_input;
	struct wl_listener cursor_motion;

	/* Includes disabled outputs. */
	struct wl_list outputs; // cg_output::link
	struct wlr_output_layout *output_layout;
	struct wl_listener new_output;

	struct wl_listener new_xdg_shell_surface;

	struct wl_listener view_mapped;
	struct wl_listener view_unmapped;
};

#endif
