#ifndef CG_SERVER_H
#define CG_SERVER_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>

#include "desktop/output.h"

struct cg_server {
	struct wl_display *wl_display;

	/* Includes disabled outputs. */
	struct wl_list outputs; // cg_output::link
	struct wlr_output_layout *output_layout;
	struct wl_listener new_output;
};

#endif
