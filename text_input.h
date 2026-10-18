/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#ifndef CG_TEXT_INPUT_H
#define CG_TEXT_INPUT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>

struct cg_server;

struct cg_text_input_relay {
	struct cg_server *server;

	struct wlr_text_input_manager_v3 *text_input_manager;
	struct wlr_input_method_manager_v2 *input_method_manager;

	struct wl_list text_inputs; // struct cg_text_input.link
	struct wlr_input_method_v2 *input_method;
	bool input_method_active;
	struct wl_event_source *deactivate_timer;

	struct wl_listener text_input_new;
	struct wl_listener text_input_manager_destroy;

	struct wl_listener input_method_new;
	struct wl_listener input_method_manager_destroy;
	struct wl_listener input_method_commit;
	struct wl_listener input_method_destroy;
	struct wl_listener input_method_grab_keyboard;
	struct wl_listener input_method_keyboard_grab_destroy;
};

struct cg_text_input {
	struct cg_text_input_relay *relay;
	struct wlr_text_input_v3 *input;
	struct wlr_surface *pending_focused_surface;

	struct wl_list link;

	struct wl_listener pending_focused_surface_destroy;
	struct wl_listener text_input_enable;
	struct wl_listener text_input_commit;
	struct wl_listener text_input_disable;
	struct wl_listener text_input_destroy;
};

void text_input_relay_init(struct cg_server *server, struct cg_text_input_relay *relay);
void text_input_relay_finish(struct cg_text_input_relay *relay);
void text_input_relay_set_focus(struct cg_text_input_relay *relay, struct wlr_surface *surface);

#endif
