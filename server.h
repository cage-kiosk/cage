#ifndef CG_SERVER_H
#define CG_SERVER_H

#include "config.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "output.h"
#include "seat.h"
#include "view.h"

enum cg_multi_output_mode {
	CAGE_MULTI_OUTPUT_MODE_EXTEND,
	CAGE_MULTI_OUTPUT_MODE_LAST,
};

struct cg_server {
	struct wl_display *wl_display;
	struct wl_list views;
	struct wlr_backend *backend;

	struct cg_seat *seat;
	struct wlr_idle *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_v1;
	struct wl_listener new_idle_inhibitor_v1;
	struct wl_list inhibitors;

	enum cg_multi_output_mode output_mode;
	struct wlr_output_layout *output_layout;
	/* Includes disabled outputs; depending on the output_mode
	 * some outputs may be disabled. */
	struct wl_list outputs; // cg_output::link
	struct wl_listener new_output;

	struct wl_listener xdg_toplevel_decoration;
	struct wl_listener new_xdg_shell_surface;
#if CAGE_HAS_XWAYLAND
	struct wl_listener new_xwayland_surface;
#endif

	bool xdg_decoration;
	bool allow_vt_switch;
	enum wl_output_transform output_transform;
#ifdef DEBUG
	bool debug_damage_tracking;
#endif
    bool return_app_code;
};

#endif
