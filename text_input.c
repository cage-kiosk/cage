/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "text_input.h"
#include "seat.h"
#include "server.h"

#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/log.h>

static struct cg_text_input *
relay_get_focused_text_input(struct cg_text_input_relay *relay)
{
	struct cg_text_input *text_input;
	wl_list_for_each (text_input, &relay->text_inputs, link) {
		if (text_input->input->focused_surface) {
			return text_input;
		}
	}
	return NULL;
}

static void
handle_im_commit(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_commit);

	struct cg_text_input *text_input = relay_get_focused_text_input(relay);
	if (!text_input) {
		return;
	}

	if (relay->input_method->current.preedit.text) {
		wlr_text_input_v3_send_preedit_string(text_input->input, relay->input_method->current.preedit.text,
						      relay->input_method->current.preedit.cursor_begin,
						      relay->input_method->current.preedit.cursor_end);
	}
	if (relay->input_method->current.commit_text) {
		wlr_text_input_v3_send_commit_string(text_input->input, relay->input_method->current.commit_text);
	}
	if (relay->input_method->current.delete.before_length || relay->input_method->current.delete.after_length) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input->input,
							       relay->input_method->current.delete.before_length,
							       relay->input_method->current.delete.after_length);
	}
	wlr_text_input_v3_send_done(text_input->input);
}

static void
handle_im_keyboard_grab_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_keyboard_grab_destroy);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = relay->input_method->keyboard_grab;
	struct wlr_seat *wlr_seat = keyboard_grab->input_method->seat;
	wl_list_remove(&relay->input_method_keyboard_grab_destroy.link);

	if (keyboard_grab->keyboard) {
		wlr_seat_set_keyboard(wlr_seat, keyboard_grab->keyboard);
		wlr_seat_keyboard_notify_modifiers(wlr_seat, &keyboard_grab->keyboard->modifiers);
	}
}

static void
handle_im_grab_keyboard(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;

	if (!relay->server->seat) {
		return;
	}

	struct wlr_keyboard *active_keyboard = wlr_seat_get_keyboard(relay->server->seat->seat);
	wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, active_keyboard);

	wl_signal_add(&keyboard_grab->events.destroy, &relay->input_method_keyboard_grab_destroy);
	relay->input_method_keyboard_grab_destroy.notify = handle_im_keyboard_grab_destroy;
}

static void
text_input_set_pending_focused_surface(struct cg_text_input *text_input, struct wlr_surface *surface)
{
	wl_list_remove(&text_input->pending_focused_surface_destroy.link);
	text_input->pending_focused_surface = surface;

	if (surface) {
		wl_signal_add(&surface->events.destroy, &text_input->pending_focused_surface_destroy);
	} else {
		wl_list_init(&text_input->pending_focused_surface_destroy.link);
	}
}

static void
handle_im_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_destroy);
	wl_list_remove(&relay->input_method_commit.link);
	wl_list_remove(&relay->input_method_grab_keyboard.link);
	wl_list_remove(&relay->input_method_destroy.link);
	relay->input_method = NULL;

	wlr_log(WLR_INFO, "Input method disconnected");

	struct cg_text_input *text_input;
	wl_list_for_each (text_input, &relay->text_inputs, link) {
		if (text_input->input->focused_surface) {
			text_input_set_pending_focused_surface(text_input, text_input->input->focused_surface);
			wlr_text_input_v3_send_leave(text_input->input);
		}
	}
}

static void
relay_send_im_state(struct cg_text_input_relay *relay, struct wlr_text_input_v3 *input)
{
	struct wlr_input_method_v2 *input_method = relay->input_method;
	if (!input_method) {
		return;
	}

	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(input_method, input->current.surrounding.text,
							  input->current.surrounding.cursor,
							  input->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(input_method, input->current.text_change_cause);
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(input_method, input->current.content_type.hint,
						      input->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(input_method);
}

static void
handle_text_input_enable(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_enable);
	if (text_input->input->focused_surface == NULL) {
		return;
	}
	if (text_input->relay->input_method == NULL) {
		return;
	}
	wlr_log(WLR_INFO, "Text input v3 enabled -> activating input method");
	wlr_input_method_v2_send_activate(text_input->relay->input_method);
	relay_send_im_state(text_input->relay, text_input->input);
}

static void
handle_text_input_commit(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_commit);
	if (text_input->input->focused_surface == NULL || !text_input->input->current_enabled) {
		return;
	}
	if (text_input->relay->input_method == NULL) {
		return;
	}
	relay_send_im_state(text_input->relay, text_input->input);
}

static void
relay_disable_text_input(struct cg_text_input_relay *relay, struct cg_text_input *text_input)
{
	if (relay->input_method == NULL) {
		return;
	}
	wlr_log(WLR_INFO, "Text input v3 disabled -> deactivating input method");
	wlr_input_method_v2_send_deactivate(relay->input_method);
	relay_send_im_state(relay, text_input->input);
}

static void
handle_text_input_disable(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_disable);
	if (text_input->input->focused_surface == NULL) {
		return;
	}
	relay_disable_text_input(text_input->relay, text_input);
}

static void
handle_text_input_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_destroy);

	if (text_input->input->current_enabled) {
		relay_disable_text_input(text_input->relay, text_input);
	}
	text_input_set_pending_focused_surface(text_input, NULL);
	wl_list_remove(&text_input->text_input_commit.link);
	wl_list_remove(&text_input->text_input_destroy.link);
	wl_list_remove(&text_input->text_input_disable.link);
	wl_list_remove(&text_input->text_input_enable.link);
	wl_list_remove(&text_input->link);
	free(text_input);
}

static void
handle_pending_focused_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, pending_focused_surface_destroy);
	text_input->pending_focused_surface = NULL;
	wl_list_remove(&text_input->pending_focused_surface_destroy.link);
	wl_list_init(&text_input->pending_focused_surface_destroy.link);
}

static struct cg_text_input *
text_input_create(struct cg_text_input_relay *relay, struct wlr_text_input_v3 *text_input)
{
	struct cg_text_input *input = calloc(1, sizeof(*input));
	if (!input) {
		return NULL;
	}
	input->input = text_input;
	input->relay = relay;

	wl_list_insert(&relay->text_inputs, &input->link);

	input->text_input_enable.notify = handle_text_input_enable;
	wl_signal_add(&text_input->events.enable, &input->text_input_enable);

	input->text_input_commit.notify = handle_text_input_commit;
	wl_signal_add(&text_input->events.commit, &input->text_input_commit);

	input->text_input_disable.notify = handle_text_input_disable;
	wl_signal_add(&text_input->events.disable, &input->text_input_disable);

	input->text_input_destroy.notify = handle_text_input_destroy;
	wl_signal_add(&text_input->events.destroy, &input->text_input_destroy);

	input->pending_focused_surface_destroy.notify = handle_pending_focused_surface_destroy;
	wl_list_init(&input->pending_focused_surface_destroy.link);

	return input;
}

static void
relay_handle_text_input(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, text_input_new);
	struct wlr_text_input_v3 *wlr_text_input = data;
	if (!relay->server->seat || relay->server->seat->seat != wlr_text_input->seat) {
		return;
	}

	wlr_log(WLR_INFO, "New text input v3 created");

	struct cg_text_input *input = text_input_create(relay, wlr_text_input);
	if (!input) {
		return;
	}

	struct wlr_surface *focused_surface = relay->server->seat->seat->keyboard_state.focused_surface;
	if (focused_surface &&
	    wl_resource_get_client(wlr_text_input->resource) == wl_resource_get_client(focused_surface->resource)) {
		if (relay->input_method) {
			wlr_text_input_v3_send_enter(wlr_text_input, focused_surface);
		} else {
			text_input_set_pending_focused_surface(input, focused_surface);
		}
	}
}

static void
relay_handle_input_method(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_new);
	struct wlr_input_method_v2 *input_method = data;
	if (!relay->server->seat || relay->server->seat->seat != input_method->seat) {
		return;
	}

	if (relay->input_method != NULL) {
		wlr_log(WLR_INFO, "Attempted to connect second input method to seat");
		wlr_input_method_v2_send_unavailable(input_method);
		return;
	}

	relay->input_method = input_method;
	wl_signal_add(&relay->input_method->events.commit, &relay->input_method_commit);
	relay->input_method_commit.notify = handle_im_commit;
	wl_signal_add(&relay->input_method->events.grab_keyboard, &relay->input_method_grab_keyboard);
	relay->input_method_grab_keyboard.notify = handle_im_grab_keyboard;
	wl_signal_add(&relay->input_method->events.destroy, &relay->input_method_destroy);
	relay->input_method_destroy.notify = handle_im_destroy;

	wlr_log(WLR_INFO, "Input method connected");

	struct cg_text_input *text_input;
	wl_list_for_each (text_input, &relay->text_inputs, link) {
		if (text_input->pending_focused_surface) {
			wlr_text_input_v3_send_enter(text_input->input, text_input->pending_focused_surface);
			text_input_set_pending_focused_surface(text_input, NULL);
		}
	}
}

static void
relay_handle_text_input_manager_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, text_input_manager_destroy);
	wl_list_remove(&relay->text_input_new.link);
	wl_list_remove(&relay->text_input_manager_destroy.link);
	wl_list_init(&relay->text_input_new.link);
	wl_list_init(&relay->text_input_manager_destroy.link);
	relay->text_input_manager = NULL;
}

static void
relay_handle_input_method_manager_destroy(struct wl_listener *listener, void *data)
{
	struct cg_text_input_relay *relay = wl_container_of(listener, relay, input_method_manager_destroy);
	wl_list_remove(&relay->input_method_new.link);
	wl_list_remove(&relay->input_method_manager_destroy.link);
	wl_list_init(&relay->input_method_new.link);
	wl_list_init(&relay->input_method_manager_destroy.link);
	relay->input_method_manager = NULL;
}

void
text_input_relay_init(struct cg_server *server, struct cg_text_input_relay *relay)
{
	relay->server = server;
	wl_list_init(&relay->text_inputs);

	relay->text_input_manager = wlr_text_input_manager_v3_create(server->wl_display);
	if (relay->text_input_manager) {
		relay->text_input_new.notify = relay_handle_text_input;
		wl_signal_add(&relay->text_input_manager->events.new_text_input, &relay->text_input_new);
		relay->text_input_manager_destroy.notify = relay_handle_text_input_manager_destroy;
		wl_signal_add(&relay->text_input_manager->events.destroy, &relay->text_input_manager_destroy);
	} else {
		wlr_log(WLR_ERROR, "Failed to create text_input_manager_v3");
	}

	relay->input_method_manager = wlr_input_method_manager_v2_create(server->wl_display);
	if (relay->input_method_manager) {
		relay->input_method_new.notify = relay_handle_input_method;
		wl_signal_add(&relay->input_method_manager->events.new_input_method, &relay->input_method_new);
		relay->input_method_manager_destroy.notify = relay_handle_input_method_manager_destroy;
		wl_signal_add(&relay->input_method_manager->events.destroy, &relay->input_method_manager_destroy);
	} else {
		wlr_log(WLR_ERROR, "Failed to create input_method_manager_v2");
	}
}

void
text_input_relay_finish(struct cg_text_input_relay *relay)
{
	if (relay->text_input_manager) {
		wl_list_remove(&relay->text_input_new.link);
		wl_list_remove(&relay->text_input_manager_destroy.link);
	}
	if (relay->input_method_manager) {
		wl_list_remove(&relay->input_method_new.link);
		wl_list_remove(&relay->input_method_manager_destroy.link);
	}
	if (relay->input_method) {
		wl_list_remove(&relay->input_method_commit.link);
		wl_list_remove(&relay->input_method_grab_keyboard.link);
		wl_list_remove(&relay->input_method_destroy.link);
		relay->input_method = NULL;
	}

	struct cg_text_input *text_input, *tmp;
	wl_list_for_each_safe (text_input, tmp, &relay->text_inputs, link) {
		text_input_set_pending_focused_surface(text_input, NULL);
		wl_list_remove(&text_input->text_input_commit.link);
		wl_list_remove(&text_input->text_input_destroy.link);
		wl_list_remove(&text_input->text_input_disable.link);
		wl_list_remove(&text_input->text_input_enable.link);
		wl_list_remove(&text_input->link);
		free(text_input);
	}
}

void
text_input_relay_set_focus(struct cg_text_input_relay *relay, struct wlr_surface *surface)
{
	wlr_log(WLR_INFO, "Text input relay set focus: surface=%p", surface);

	struct cg_text_input *text_input;
	wl_list_for_each (text_input, &relay->text_inputs, link) {
		if (text_input->pending_focused_surface) {
			assert(text_input->input->focused_surface == NULL);
			if (surface != text_input->pending_focused_surface) {
				text_input_set_pending_focused_surface(text_input, NULL);
			}
		} else if (text_input->input->focused_surface) {
			assert(text_input->pending_focused_surface == NULL);
			if (surface != text_input->input->focused_surface) {
				wlr_text_input_v3_send_leave(text_input->input);
				relay_disable_text_input(relay, text_input);
			} else {
				continue;
			}
		}

		if (surface &&
		    wl_resource_get_client(text_input->input->resource) == wl_resource_get_client(surface->resource)) {
			if (relay->input_method) {
				wlr_text_input_v3_send_enter(text_input->input, surface);
			} else {
				text_input_set_pending_focused_surface(text_input, surface);
			}
		}
	}
}
