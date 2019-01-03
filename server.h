#ifndef CG_SERVER_H
#define CG_SERVER_H

#include "config.h"

#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output_layout.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "output.h"
#include "seat.h"

struct cg_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;

	struct wl_listener new_xdg_shell_surface;
	struct wl_list views;

	struct cg_seat *seat;

	struct wlr_output_layout *output_layout;
	struct cg_output *output;
	struct wl_listener new_output;

#if CAGE_HAS_XWAYLAND
	struct wl_listener new_xwayland_surface;
#endif
};

#endif
