/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2025 Cage Authors
 * Copyright (C) 2020-2024 Sway Authors
 *
 * See the LICENSE file accompanying this file.
 */

#ifndef CG_IME_H
#define CG_IME_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>

/**
 * The relay structure manages the relationship between text-input and
 * input_method interfaces on a given seat. Multiple text-input interfaces may
 * be bound to a relay, but at most one will be focused (receiving events) at
 * a time. At most one input-method interface may be bound to the seat. The
 * relay manages life cycle of both sides. When both sides are present and
 * focused, the relay passes messages between them.
 *
 * Text input focus is a subset of keyboard focus - if the text-input is
 * in the focused state, wl_keyboard sent an enter as well. However, having
 * wl_keyboard focused doesn't mean that text-input will be focused.
 */
struct cg_ime_relay {
	struct cg_seat *seat;

	struct wl_list text_inputs; // cg_text_input::link
	struct wl_list input_popups; // cg_input_popup::link
	struct wlr_input_method_v2 *input_method; // doesn't have to be present
	struct wl_array pressed_keys;

	struct wl_listener input_method_commit;
	struct wl_listener input_method_new_popup_surface;
	struct wl_listener input_method_grab_keyboard;
	struct wl_listener input_method_destroy;

	struct wl_listener input_method_keyboard_grab_destroy;
};

struct cg_text_input {
	struct cg_ime_relay *relay;

	struct wlr_text_input_v3 *input;
	// The surface getting seat's focus. Stored for when text-input cannot
	// be sent an enter event immediately after getting focus, e.g. when
	// there's no input method available. Cleared once text-input is entered.
	struct wlr_surface *pending_focused_surface;

	struct wl_list link;

	struct wl_listener pending_focused_surface_destroy;

	struct wl_listener text_input_enable;
	struct wl_listener text_input_commit;
	struct wl_listener text_input_disable;
	struct wl_listener text_input_destroy;
};

struct cg_input_popup {
	struct cg_ime_relay *relay;

	struct wlr_scene_node *focused_scene_node;
	struct cg_view *focused_view;
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_output *fixed_output;

	struct cg_view *view;

	struct wl_list link;

	struct wl_listener popup_destroy;
	struct wl_listener popup_surface_commit;
	struct wl_listener popup_surface_map;
	struct wl_listener popup_surface_unmap;

	struct wl_listener focused_surface_unmap;
};

void cg_ime_handle_new_input_method(struct wl_listener *listener, void *data);

void cg_ime_handle_new_text_input(struct wl_listener *listener, void *data);

void cg_ime_relay_init(struct cg_seat *seat, struct cg_ime_relay *relay);

void cg_ime_relay_finish(struct cg_ime_relay *relay);

// Updates currently focused surface. Surface must belong to the same seat.
void cg_ime_relay_set_focus(struct cg_ime_relay *relay, struct wlr_surface *surface);

bool cg_ime_keyboard_grab_forward_key(struct cg_ime_relay *relay, struct wlr_keyboard *keyboard,
				      struct wlr_keyboard_key_event *event);

#endif
