/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2021 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

#define OUTPUT_CONFIG_UPDATED                                                                                          \
	(WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM |      \
	 WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)

static void
update_output_manager_config(struct cg_server *server)
{
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();

	struct cg_output *output;
	wl_list_for_each (output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		struct wlr_output_configuration_head_v1 *config_head =
			wlr_output_configuration_head_v1_create(config, wlr_output);
		struct wlr_box output_box;

		wlr_output_layout_get_box(server->output_layout, wlr_output, &output_box);
		if (!wlr_box_empty(&output_box)) {
			config_head->state.x = output_box.x;
			config_head->state.y = output_box.y;
		}
	}

	wlr_output_manager_v1_set_configuration(server->output_manager_v1, config);
}

static inline void
output_layout_add_auto(struct cg_output *output)
{
	assert(output->scene_output != NULL);
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(output->server->output_layout, output->wlr_output);
	wlr_scene_output_layout_add_output(output->server->scene_output_layout, layout_output, output->scene_output);
}

static inline void
output_layout_add(struct cg_output *output, int32_t x, int32_t y)
{
	assert(output->scene_output != NULL);
	bool exists = wlr_output_layout_get(output->server->output_layout, output->wlr_output);
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add(output->server->output_layout, output->wlr_output, x, y);
	if (exists) {
		return;
	}
	wlr_scene_output_layout_add_output(output->server->scene_output_layout, layout_output, output->scene_output);
}

static inline void
output_layout_remove(struct cg_output *output)
{
	wlr_output_layout_remove(output->server->output_layout, output->wlr_output);
}

static void
output_enable(struct cg_output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;

	/* Outputs get enabled by the backend before firing the new_output event,
	 * so we can't do a check for already enabled outputs here unless we
	 * duplicate the enabled property in cg_output. */
	wlr_log(WLR_DEBUG, "Enabling output %s", wlr_output->name);

	struct wlr_output_state state = {0};
	wlr_output_state_set_enabled(&state, true);

	if (wlr_output_commit_state(wlr_output, &state)) {
		output_layout_add_auto(output);
	}

	update_output_manager_config(output->server);
}

static void
output_disable(struct cg_output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;
	if (!wlr_output->enabled) {
		wlr_log(WLR_DEBUG, "Not disabling already disabled output %s", wlr_output->name);
		return;
	}

	wlr_log(WLR_DEBUG, "Disabling output %s", wlr_output->name);
	struct wlr_output_state state = {0};
	wlr_output_state_set_enabled(&state, false);
	wlr_output_commit_state(wlr_output, &state);
	output_layout_remove(output);
}

static void
handle_output_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, frame);

	if (!output->wlr_output->enabled || !output->scene_output) {
		return;
	}

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now = {0};
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void
handle_output_commit(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;

	/* Notes:
	 * - output layout change will also be called if needed to position the views
	 * - always update output manager configuration even if the output is now disabled */

	if (event->state->committed & OUTPUT_CONFIG_UPDATED) {
		update_output_manager_config(output->server);
	}
}

static void
handle_output_request_state(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, request_state);
	struct wlr_output_event_request_state *event = data;

	if (wlr_output_commit_state(output->wlr_output, event->state)) {
		update_output_manager_config(output->server);
	}
}

void
handle_output_layout_change(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, output_layout_change);

	view_position_all(server);
	update_output_manager_config(server);
}

static bool
is_nested_output(struct cg_output *output)
{
	if (wlr_output_is_wl(output->wlr_output)) {
		return true;
	}
#if WLR_HAS_X11_BACKEND
	if (wlr_output_is_x11(output->wlr_output)) {
		return true;
	}
#endif
	return false;
}

static void
output_destroy(struct cg_output *output)
{
	struct cg_server *server = output->server;
	bool was_nested_output = is_nested_output(output);

	output->wlr_output->data = NULL;

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->link);

	output_layout_remove(output);

	free(output);

	if (wl_list_empty(&server->outputs) && was_nested_output) {
		server_terminate(server);
	} else if (server->output_mode == CAGE_MULTI_OUTPUT_MODE_LAST && !wl_list_empty(&server->outputs)) {
		struct cg_output *prev = wl_container_of(server->outputs.next, prev, link);
		output_enable(prev);
		view_position_all(server);
	}
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, destroy);
	output_destroy(output);
}

void
handle_new_output(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (wlr_output->non_desktop) {
		wlr_log(WLR_DEBUG, "Not configuring non-desktop output: %s", wlr_output->name);
#if WLR_HAS_DRM_BACKEND
		if (server->drm_lease_v1) {
			wlr_drm_lease_v1_manager_offer_output(server->drm_lease_v1, wlr_output);
		}
#endif
		return;
	}

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Failed to initialize output rendering");
		return;
	}

	struct cg_output *output = calloc(1, sizeof(struct cg_output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output");
		return;
	}

	output->wlr_output = wlr_output;
	wlr_output->data = output;
	output->server = server;

	wl_list_insert(&server->outputs, &output->link);

	output->commit.notify = handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->request_state.notify = handle_output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (!output->scene_output) {
		wlr_log(WLR_ERROR, "Failed to allocate scene output");
		return;
	}

	struct wlr_output_state state = {0};
	wlr_output_state_set_enabled(&state, true);
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *preferred_mode = wlr_output_preferred_mode(wlr_output);
		if (preferred_mode) {
			wlr_output_state_set_mode(&state, preferred_mode);
		}
		if (!wlr_output_test_state(wlr_output, &state)) {
			struct wlr_output_mode *mode;
			wl_list_for_each (mode, &wlr_output->modes, link) {
				if (mode == preferred_mode) {
					continue;
				}

				wlr_output_state_set_mode(&state, mode);
				if (wlr_output_test_state(wlr_output, &state)) {
					break;
				}
			}
		}
	}

	if (server->output_mode == CAGE_MULTI_OUTPUT_MODE_LAST && wl_list_length(&server->outputs) > 1) {
		struct cg_output *next = wl_container_of(output->link.next, next, link);
		output_disable(next);
	}

	if (!wlr_xcursor_manager_load(server->seat->xcursor_manager, wlr_output->scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for output '%s' with scale %f", wlr_output->name,
			wlr_output->scale);
	}

	wlr_log(WLR_DEBUG, "Enabling new output %s", wlr_output->name);
	if (wlr_output_commit_state(wlr_output, &state)) {
		output_layout_add_auto(output);
	}

	view_position_all(output->server);
	update_output_manager_config(output->server);
}

void
output_set_window_title(struct cg_output *output, const char *title)
{
	struct wlr_output *wlr_output = output->wlr_output;

	if (!wlr_output->enabled) {
		wlr_log(WLR_DEBUG, "Not setting window title for disabled output %s", wlr_output->name);
		return;
	}

	if (wlr_output_is_wl(wlr_output)) {
		wlr_wl_output_set_title(wlr_output, title);
#if WLR_HAS_X11_BACKEND
	} else if (wlr_output_is_x11(wlr_output)) {
		wlr_x11_output_set_title(wlr_output, title);
#endif
	}
}

static bool
output_config_apply(struct cg_server *server, struct wlr_output_configuration_v1 *config, bool test_only)
{
	bool ok = false;

	size_t states_len;
	struct wlr_backend_output_state *states = wlr_output_configuration_v1_build_state(config, &states_len);
	if (states == NULL) {
		return false;
	}

	struct wlr_output_swapchain_manager swapchain_manager;
	wlr_output_swapchain_manager_init(&swapchain_manager, server->backend);

	ok = wlr_output_swapchain_manager_prepare(&swapchain_manager, states, states_len);
	if (!ok || test_only) {
		goto out;
	}

	for (size_t i = 0; i < states_len; i++) {
		struct wlr_backend_output_state *backend_state = &states[i];
		struct cg_output *output = backend_state->output->data;

		struct wlr_swapchain *swapchain =
			wlr_output_swapchain_manager_get_swapchain(&swapchain_manager, backend_state->output);
		struct wlr_scene_output_state_options options = {
			.swapchain = swapchain,
		};
		struct wlr_output_state *state = &backend_state->base;
		if (!wlr_scene_output_build_state(output->scene_output, state, &options)) {
			ok = false;
			goto out;
		}
	}

	ok = wlr_backend_commit(server->backend, states, states_len);
	if (!ok) {
		goto out;
	}

	wlr_output_swapchain_manager_apply(&swapchain_manager);

	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each (head, &config->heads, link) {
		struct cg_output *output = head->state.output->data;

		if (head->state.enabled) {
			output_layout_add(output, head->state.x, head->state.y);
		} else {
			output_layout_remove(output);
		}
	}

out:
	wlr_output_swapchain_manager_finish(&swapchain_manager);
	for (size_t i = 0; i < states_len; i++) {
		wlr_output_state_finish(&states[i].base);
	}
	free(states);
	return ok;
}

void
handle_output_manager_apply(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	if (output_config_apply(server, config, false)) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}

	wlr_output_configuration_v1_destroy(config);
}

void
handle_output_manager_test(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;

	if (output_config_apply(server, config, true)) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}

	wlr_output_configuration_v1_destroy(config);
}
