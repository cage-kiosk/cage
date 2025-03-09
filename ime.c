/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2025 Cage Authors
 * Copyright (C) 2020-2024 Sway Authors
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "ime.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>

static struct cg_text_input *
relay_get_focusable_text_input(struct cg_ime_relay *relay)
{
	struct cg_text_input *text_input = NULL;
	wl_list_for_each (text_input, &relay->text_inputs, link) {
		if (text_input->pending_focused_surface) {
			return text_input;
		}
	}
	return NULL;
}

static struct cg_text_input *
relay_get_focused_text_input(struct cg_ime_relay *relay)
{
	struct cg_text_input *text_input = NULL;
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
	struct cg_ime_relay *relay = wl_container_of(listener, relay, input_method_commit);

	struct cg_text_input *text_input = relay_get_focused_text_input(relay);
	if (!text_input) {
		return;
	}
	struct wlr_input_method_v2 *context = data;
	assert(context == relay->input_method);
	if (context->current.preedit.text) {
		wlr_text_input_v3_send_preedit_string(text_input->input, context->current.preedit.text,
						      context->current.preedit.cursor_begin,
						      context->current.preedit.cursor_end);
	}
	if (context->current.commit_text) {
		wlr_text_input_v3_send_commit_string(text_input->input, context->current.commit_text);
	}
	if (context->current.delete.before_length || context->current.delete.after_length) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input->input, context->current.delete.before_length,
							       context->current.delete.after_length);
	}
	wlr_text_input_v3_send_done(text_input->input);
}

static void
handle_im_keyboard_grab_destroy(struct wl_listener *listener, void *data)
{
	struct cg_ime_relay *relay = wl_container_of(listener, relay, input_method_keyboard_grab_destroy);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;
	struct wlr_seat *wlr_seat = keyboard_grab->input_method->seat;
	wl_list_remove(&relay->input_method_keyboard_grab_destroy.link);

	if (keyboard_grab->keyboard) {
		// send modifier state to original client
		wlr_seat_set_keyboard(wlr_seat, keyboard_grab->keyboard);
		wlr_seat_keyboard_notify_modifiers(wlr_seat, &keyboard_grab->keyboard->modifiers);
	}
}

static void
handle_im_grab_keyboard(struct wl_listener *listener, void *data)
{
	struct cg_ime_relay *relay = wl_container_of(listener, relay, input_method_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;

	// send modifier state to grab
	struct wlr_keyboard *active_keyboard = wlr_seat_get_keyboard(relay->seat->seat);
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
	struct cg_ime_relay *relay = wl_container_of(listener, relay, input_method_destroy);
	struct wlr_input_method_v2 *context = data;
	assert(context == relay->input_method);
	wl_list_remove(&relay->input_method_commit.link);
	wl_list_remove(&relay->input_method_grab_keyboard.link);
	wl_list_remove(&relay->input_method_destroy.link);
	wl_list_remove(&relay->input_method_new_popup_surface.link);
	relay->input_method = NULL;
	struct cg_text_input *text_input = relay_get_focused_text_input(relay);
	if (text_input) {
		// keyboard focus is still there, so keep the surface at hand in case
		// the input method returns
		text_input_set_pending_focused_surface(text_input, text_input->input->focused_surface);
		wlr_text_input_v3_send_leave(text_input->input);
	}
}

static void
constrain_popup(struct cg_input_popup *popup)
{
	struct cg_text_input *text_input = relay_get_focused_text_input(popup->relay);
	struct cg_server *server = popup->relay->seat->server;

	if (!popup->focused_scene_node) {
		return;
	}

	struct wlr_box parent = {0};
	wlr_scene_node_coords(&popup->focused_scene_node->parent->node, &parent.x, &parent.y);

	struct wlr_box geo = {0};
	struct wlr_output *output;

	if (popup->focused_view) {
		struct cg_view *view = popup->focused_view;

		output = wlr_output_layout_output_at(server->output_layout, view->lx, view->ly);

		view->impl->get_geometry(view, &parent.width, &parent.height);

		geo.x = view->lx;
		geo.y = view->ly;
		geo.width = parent.width;
		geo.height = parent.height;
	} else {
		output = popup->fixed_output;
	}

	struct wlr_box output_box;
	wlr_output_layout_get_box(server->output_layout, output, &output_box);

	bool cursor_rect = text_input->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE;
	struct wlr_box cursor_area;
	if (cursor_rect) {
		cursor_area = text_input->input->current.cursor_rectangle;
	} else {
		cursor_area = (struct wlr_box) {
			.width = parent.width,
			.height = parent.height,
		};
	}

	int popup_width = popup->popup_surface->surface->current.width;
	int popup_height = popup->popup_surface->surface->current.height;
	int x1 = parent.x + cursor_area.x;
	int x2 = parent.x + cursor_area.x + cursor_area.width;
	int y1 = parent.y + cursor_area.y;
	int y2 = parent.y + cursor_area.y + cursor_area.height;
	int x = x1;
	int y = y2;

	int available_right = output_box.x + output_box.width - x1;
	int available_left = x2 - output_box.x;
	if (available_right < popup_width && available_left > available_right) {
		x = x2 - popup_width;
	}

	int available_down = output_box.y + output_box.height - y2;
	int available_up = y1 - output_box.y;
	if (available_down < popup_height && available_up > available_down) {
		y = y1 - popup_height;
	}

	wlr_scene_node_set_position(popup->focused_scene_node, x - parent.x - geo.x, y - parent.y - geo.y);
	if (cursor_rect) {
		struct wlr_box box = {
			.x = x1 - x,
			.y = y1 - y,
			.width = cursor_area.width,
			.height = cursor_area.height,
		};
		wlr_input_popup_surface_v2_send_text_input_rectangle(popup->popup_surface, &box);
	}

	if (popup->view->scene_tree) {
		wlr_scene_node_set_position(&popup->view->scene_tree->node, x - geo.x, y - geo.y);
	}
}

static void input_popup_set_focus(struct cg_input_popup *popup, struct wlr_surface *surface);

static void
relay_send_im_state(struct cg_ime_relay *relay, struct wlr_text_input_v3 *input)
{
	struct wlr_input_method_v2 *input_method = relay->input_method;
	if (!input_method) {
		wlr_log(WLR_INFO, "Sending IM_DONE but im is gone");
		return;
	}
	// TODO: only send each of those if they were modified
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

	struct cg_text_input *text_input = relay_get_focused_text_input(relay);

	struct cg_input_popup *popup;
	wl_list_for_each (popup, &relay->input_popups, link) {
		if (text_input != NULL) {
			input_popup_set_focus(popup, text_input->input->focused_surface);
		} else {
			input_popup_set_focus(popup, NULL);
		}
	}
	wlr_input_method_v2_send_done(input_method);
	// TODO: pass intent, display popup size
}

static void
handle_text_input_enable(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_enable);
	if (text_input->relay->input_method == NULL) {
		wlr_log(WLR_INFO, "Enabling text input when input method is gone");
		return;
	}
	wlr_input_method_v2_send_activate(text_input->relay->input_method);
	relay_send_im_state(text_input->relay, text_input->input);
}

static void
handle_text_input_commit(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_commit);
	if (!text_input->input->current_enabled) {
		wlr_log(WLR_INFO, "Inactive text input tried to commit an update");
		return;
	}
	wlr_log(WLR_DEBUG, "Text input committed update");
	if (text_input->relay->input_method == NULL) {
		wlr_log(WLR_INFO, "Text input committed, but input method is gone");
		return;
	}
	relay_send_im_state(text_input->relay, text_input->input);
}

static void
relay_disable_text_input(struct cg_ime_relay *relay, struct cg_text_input *text_input)
{
	if (relay->input_method == NULL) {
		wlr_log(WLR_DEBUG, "Disabling text input, but input method is gone");
		return;
	}
	wlr_input_method_v2_send_deactivate(relay->input_method);
	relay_send_im_state(relay, text_input->input);
}

static void
handle_text_input_disable(struct wl_listener *listener, void *data)
{
	struct cg_text_input *text_input = wl_container_of(listener, text_input, text_input_disable);
	if (text_input->input->focused_surface == NULL) {
		wlr_log(WLR_DEBUG, "Disabling text input, but no longer focused");
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
	struct wlr_surface *surface = data;
	assert(text_input->pending_focused_surface == surface);
	text_input->pending_focused_surface = NULL;
	wl_list_remove(&text_input->pending_focused_surface_destroy.link);
	wl_list_init(&text_input->pending_focused_surface_destroy.link);
}

static struct cg_text_input *
cg_text_input_create(struct cg_ime_relay *relay, struct wlr_text_input_v3 *text_input)
{
	struct cg_text_input *input = calloc(1, sizeof(struct cg_text_input));
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

void
cg_ime_handle_new_text_input(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_text_input);
	struct cg_ime_relay *relay = &server->seat->ime_relay;
	struct wlr_text_input_v3 *wlr_text_input = data;
	if (relay->seat->seat != wlr_text_input->seat) {
		return;
	}

	cg_text_input_create(relay, wlr_text_input);
}

static void
input_popup_set_focus(struct cg_input_popup *popup, struct wlr_surface *surface)
{
	wl_list_remove(&popup->focused_surface_unmap.link);

	if (!popup->view->scene_tree) {
		wl_list_init(&popup->focused_surface_unmap.link);
		return;
	}

	if (popup->focused_scene_node) {
		wlr_scene_node_destroy(popup->focused_scene_node);
		popup->focused_scene_node = NULL;
	}

	if (surface == NULL) {
		wl_list_init(&popup->focused_surface_unmap.link);
		wlr_scene_node_set_enabled(&popup->view->scene_tree->node, false);
		return;
	}

	struct cg_view *view = view_from_wlr_surface(surface);
	wl_signal_add(&view->wlr_surface->events.unmap, &popup->focused_surface_unmap);
	popup->focused_view = view;

	struct wlr_scene_tree *relative = wlr_scene_tree_create(view->scene_tree);

	popup->focused_scene_node = &relative->node;

	constrain_popup(popup);
	wlr_scene_node_set_enabled(&popup->view->scene_tree->node, true);
}

static void
handle_im_popup_destroy(struct wl_listener *listener, void *data)
{
	struct cg_input_popup *popup = wl_container_of(listener, popup, popup_destroy);
	wl_list_remove(&popup->focused_surface_unmap.link);
	wl_list_remove(&popup->popup_surface_commit.link);
	wl_list_remove(&popup->popup_surface_map.link);
	wl_list_remove(&popup->popup_surface_unmap.link);
	wl_list_remove(&popup->popup_destroy.link);
	wl_list_remove(&popup->link);

	view_destroy(popup->view);
	free(popup);
}

static void
handle_im_popup_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_input_popup *popup = wl_container_of(listener, popup, popup_surface_map);
	struct cg_text_input *text_input = relay_get_focused_text_input(popup->relay);
	view_map(popup->view, popup->popup_surface->surface);
	if (text_input != NULL) {
		input_popup_set_focus(popup, text_input->input->focused_surface);
	} else {
		input_popup_set_focus(popup, NULL);
	}
}

static void
handle_im_popup_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_input_popup *popup = wl_container_of(listener, popup, popup_surface_unmap);

	// relative should already be freed as it should be a child of the just unmapped scene
	popup->focused_scene_node = NULL;
	view_unmap(popup->view);

	input_popup_set_focus(popup, NULL);
}

static void
handle_im_popup_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_input_popup *popup = wl_container_of(listener, popup, popup_surface_commit);

	constrain_popup(popup);
}

static void
handle_im_focused_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_input_popup *popup = wl_container_of(listener, popup, focused_surface_unmap);

	input_popup_set_focus(popup, NULL);
}

static char *
get_title(struct cg_view *view)
{
	return strdup("Input popup");
}

static void
get_geometry(struct cg_view *view, int *width, int *height)
{
	if (view->wlr_surface) {
		*width = view->wlr_surface->current.width;
		*height = view->wlr_surface->current.height;
	} else {
		*width = 0;
		*height = 0;
	}
}

static void
activate(struct cg_view *view, bool activate)
{
	// no-op
	// wlr_scene_node_set_enabled(&view->scene_tree->node, activate);
}

static void
maximize(struct cg_view *view, int width, int height)
{
	// no-op
}

static void
input_popup_view_destroy(struct cg_view *view)
{
	free(view);
}

static bool
is_primary(struct cg_view *view)
{
	return false;
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	return false;
}

static struct cg_view_impl input_popup_view_impl = {
	.get_title = get_title,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = activate,
	.maximize = maximize,
	.destroy = input_popup_view_destroy,
};

static void
handle_im_new_popup_surface(struct wl_listener *listener, void *data)
{
	struct cg_ime_relay *relay = wl_container_of(listener, relay, input_method_new_popup_surface);
	struct cg_input_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wlr_log(WLR_ERROR, "Failed to allocate an input method popup");
		return;
	}

	popup->relay = relay;
	popup->popup_surface = data;
	popup->popup_surface->data = popup;

	popup->view = calloc(1, sizeof(*popup->view));
	if (!popup->view) {
		wlr_log(WLR_ERROR, "Failed to allocate cg_view for input popup");
		free(popup);
		return;
	}

	view_init(popup->view, relay->seat->server, CAGE_INPUT_POPUP_VIEW, &input_popup_view_impl);

	wl_signal_add(&popup->popup_surface->events.destroy, &popup->popup_destroy);
	popup->popup_destroy.notify = handle_im_popup_destroy;
	wl_signal_add(&popup->popup_surface->surface->events.commit, &popup->popup_surface_commit);
	popup->popup_surface_commit.notify = handle_im_popup_surface_commit;
	wl_signal_add(&popup->popup_surface->surface->events.map, &popup->popup_surface_map);
	popup->popup_surface_map.notify = handle_im_popup_surface_map;
	wl_signal_add(&popup->popup_surface->surface->events.unmap, &popup->popup_surface_unmap);
	popup->popup_surface_unmap.notify = handle_im_popup_surface_unmap;
	wl_list_init(&popup->focused_surface_unmap.link);
	popup->focused_surface_unmap.notify = handle_im_focused_surface_unmap;

	struct cg_text_input *text_input = relay_get_focused_text_input(relay);
	if (text_input != NULL) {
		input_popup_set_focus(popup, text_input->input->focused_surface);
	} else {
		input_popup_set_focus(popup, NULL);
	}

	wl_list_insert(&relay->input_popups, &popup->link);
}

static void
text_input_send_enter(struct cg_text_input *text_input, struct wlr_surface *surface)
{
	wlr_text_input_v3_send_enter(text_input->input, surface);
	struct cg_input_popup *popup;
	wl_list_for_each (popup, &text_input->relay->input_popups, link) {
		input_popup_set_focus(popup, surface);
	}
}

void
cg_ime_handle_new_input_method(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_input_method);
	struct cg_ime_relay *relay = &server->seat->ime_relay;
	struct wlr_input_method_v2 *input_method = data;
	if (relay->seat->seat != input_method->seat) {
		return;
	}

	if (relay->input_method != NULL) {
		wlr_log(WLR_INFO, "Attempted to connect second input method to a seat");
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
	wl_signal_add(&relay->input_method->events.new_popup_surface, &relay->input_method_new_popup_surface);
	relay->input_method_new_popup_surface.notify = handle_im_new_popup_surface;

	struct cg_text_input *text_input = relay_get_focusable_text_input(relay);
	if (text_input) {
		text_input_send_enter(text_input, text_input->pending_focused_surface);
		text_input_set_pending_focused_surface(text_input, NULL);
	}
}

void
cg_ime_relay_init(struct cg_seat *seat, struct cg_ime_relay *relay)
{
	relay->seat = seat;
	wl_list_init(&relay->text_inputs);
	wl_list_init(&relay->input_popups);
	wl_array_init(&relay->pressed_keys);
}

void
cg_ime_relay_finish(struct cg_ime_relay *relay)
{
	wl_array_release(&relay->pressed_keys);
}

void
cg_ime_relay_set_focus(struct cg_ime_relay *relay, struct wlr_surface *surface)
{
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
				relay_disable_text_input(relay, text_input);
				wlr_text_input_v3_send_leave(text_input->input);
			} else {
				wlr_log(WLR_DEBUG, "IM relay set_focus already focused");
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

// Simple implementation of "set" data structure

static bool
pressed_keys_contains(struct wl_array *pressed_keys, uint32_t keycode)
{
	uint32_t *key;
	wl_array_for_each(key, pressed_keys)
	{
		if (*key == keycode) {
			return true;
		}
	}
	return false;
}

static void
pressed_keys_add(struct wl_array *pressed_keys, uint32_t keycode)
{
	if (pressed_keys_contains(pressed_keys, keycode)) {
		return;
	}

	uint32_t *key;
	wl_array_for_each(key, pressed_keys)
	{
		if (*key == 0) {
			*key = keycode;
			return;
		}
	}

	key = wl_array_add(pressed_keys, sizeof(*key));
	if (key) {
		*key = keycode;
	} else {
		wlr_log(WLR_ERROR, "Failed to add pressed key");
	}
}

static void
pressed_keys_remove(struct wl_array *pressed_keys, uint32_t keycode)
{
	uint32_t *key;
	wl_array_for_each(key, pressed_keys)
	{
		if (*key == keycode) {
			*key = 0;
			break;
		}
	}
}

bool
cg_ime_keyboard_grab_forward_key(struct cg_ime_relay *relay, struct wlr_keyboard *keyboard,
				 struct wlr_keyboard_key_event *event)
{
	struct wlr_input_method_v2 *input_method = relay->input_method;
	if (!input_method) {
		return false;
	}

	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = input_method->keyboard_grab;
	if (!keyboard_grab) {
		return false;
	}

	// If a key-release event is from zwp_virtual_keyboard_v1 created by IME, and there was not corresponding
	// key-press event, wlroots would not send it to clients. So we should not handle the key-release events that
	// not have corresponding key-press events, otherwise if a key was pressed before IME enabled, its release event
	// would not be sent to clients.
	assert(event->keycode != 0);
	struct wl_array *pressed_keys = &relay->pressed_keys;
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && !pressed_keys_contains(pressed_keys, event->keycode)) {
		return false;
	}

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		pressed_keys_add(pressed_keys, event->keycode);
	} else {
		pressed_keys_remove(pressed_keys, event->keycode);
	}
	wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, keyboard);
	wlr_input_method_keyboard_grab_v2_send_key(keyboard_grab, event->time_msec, event->keycode, event->state);
	return true;
}
