#ifndef CG_SERVER_H
#define CG_SERVER_H

#include "config.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

enum cg_multi_output_mode {
	CAGE_MULTI_OUTPUT_MODE_EXTEND,
	CAGE_MULTI_OUTPUT_MODE_LAST,
};

struct cg_server {
	struct wl_display *wl_display;
	struct wl_list views;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_session *session;

	struct cg_seat *seat;
	struct wlr_idle_notifier_v1 *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_v1;
	struct wl_listener new_idle_inhibitor_v1;
	struct wl_list inhibitors;

	enum cg_multi_output_mode output_mode;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_output_layout;

	struct wlr_scene *scene;
	/* Includes disabled outputs; depending on the output_mode
	 * some outputs may be disabled. */
	struct wl_list outputs; // cg_output::link
	struct wl_listener new_output;
	struct wl_listener output_layout_change;

	struct wl_listener xdg_toplevel_decoration;
	struct wl_listener new_xdg_shell_surface;

	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;

	struct wlr_layer_shell_v1 *layer_shell_v1;
	struct wl_listener new_layer_shell_v1_surface;
#if CAGE_HAS_XWAYLAND
	struct wl_listener new_xwayland_surface;
#endif
	struct wlr_output_manager_v1 *output_manager_v1;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;

	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;

	bool xdg_decoration;
	bool allow_vt_switch;
	bool return_app_code;
};

#endif
