/*
 * Cage: A Wayland kiosk.
 * 
 * Copyright (C) 2018 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

struct cg_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;

	struct wl_listener new_xdg_surface;
	struct wl_list views;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_list keyboards;

	struct wlr_output_layout *output_layout;
	struct cg_output *output;
	struct wl_listener new_output;
};

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;
};

struct cg_view {
	struct wl_list link;
	struct cg_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	bool mapped;
	int x, y;
};

struct cg_keyboard {
	struct wl_list link;
	struct cg_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct cg_server server = {0};

static void
center_view(struct cg_view *view)
{
	struct cg_server *server = view->server;
	struct wlr_output *output = server->output->wlr_output;
	int output_width, output_height;

	wlr_output_effective_resolution(output, &output_width, &output_height);

	struct wlr_box geom;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &geom);
	view->x = (output_width - geom.width) / 2;
	view->y = (output_height - geom.height) / 2;
}

static void
focus_view(struct cg_view *view)
{
	struct cg_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = view->xdg_surface->surface;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

	if (prev_surface == surface) {
		return;
	}

	if (prev_surface) {
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		wlr_xdg_toplevel_set_activated(previous, false);
	}

	/* Move the view to the front, but only if it isn't the "root" view. */
	if (view->xdg_surface->toplevel->parent != NULL) {
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
	}

	wlr_xdg_toplevel_set_activated(view->xdg_surface, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
				       keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

/* This event is raised when a modifier key, such as Shift or Alt, is
 * pressed. We simply communicate this to the client. */
static void
handle_keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
					   &keyboard->device->keyboard->modifiers);
}

static bool
handle_keybinding(struct cg_server *server, xkb_keysym_t sym)
{
	switch (sym) {
#ifdef DEBUG
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
#endif
	default:
		return false;
	}
	return true;
}

/* This event is raised when a key is pressed or released. */
static void
handle_keyboard_key(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct cg_server *server = keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate from libinput keycode to an xkbcommon keycode. */
	uint32_t keycode = event->keycode + 8;

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WLR_KEY_PRESSED) {
		/* If Alt is held down and this button was pressed, we
		 * attempt to process it as a compositor
		 * keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
					     event->keycode, event->state);
	}
}

static void
handle_keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct cg_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);

	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	free(keyboard);
}

static void
server_new_keyboard(struct cg_server *server, struct wlr_input_device *device)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Unable to create XBK context");
		return;
	}

	struct xkb_rule_names rules = { 0 };
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
							   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unable to configure keyboard: keymap does not exist");
		xkb_context_unref(context);
		return;
	}

	struct cg_keyboard *keyboard = calloc(1, sizeof(struct cg_keyboard));
	keyboard->server = server;
	keyboard->device = device;
	wlr_keyboard_set_keymap(device->keyboard, keymap);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	keyboard->modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = handle_keyboard_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = handle_keyboard_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, device);

	wl_list_insert(&server->keyboards, &keyboard->link);
}

/* This event is raised by the backend when a new input device becomes
 * available. */
static void
server_new_input(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_log(WLR_DEBUG, "Touch input is not yet implemented");
		return;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
	case WLR_INPUT_DEVICE_TABLET_PAD:
		wlr_log(WLR_DEBUG, "Tablet input is not implemented");
		return;
	}

	/* Let the wlr_seat know what our capabilities are. In Cage we
	 * always have a cursor, even if there are no pointer devices,
	 * so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* This event is raised by the seat when a client provides a cursor image. */
static void
seat_request_cursor(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
				       event->hotspot_x, event->hotspot_y);
	}
}

/* XDG toplevels may have nested surfaces, such as popup windows for context
 * menus or tooltips. This function tests if any of those are underneath the
 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
 * surface pointer to that wlr_surface and the sx and sy coordinates to the
 * coordinates relative to that surface's top-left corner. */
static bool
view_at(struct cg_view *view, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy)
{
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
	_surface = wlr_xdg_surface_surface_at(view->xdg_surface,
					      view_sx, view_sy,
					      &_sx, &_sy);

	if (_surface != NULL) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}

	return false;
}

/* This iterates over all of our surfaces and attempts to find one under the
 * cursor. This relies on server->views being ordered from top-to-bottom. */
static struct cg_view *
desktop_view_at(struct cg_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy)
{
	struct cg_view *view;

	wl_list_for_each(view, &server->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}

	return NULL;
}

/* Find the view under the pointer and send the event along. */
static void
process_cursor_motion(struct cg_server *server, uint32_t time)
{
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;

	struct cg_view *view = desktop_view_at(server,
					       server->cursor->x, server->cursor->y,
					       &surface, &sx, &sy);

	/* If desktop_view_at returns a view, there is also a
	   surface. There cannot be a surface without a view,
	   either. It's both or nothing. */
	if (!view) {
		wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "left_ptr", server->cursor);
		wlr_seat_pointer_clear_focus(seat);
	} else {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);

		bool focus_changed = seat->pointer_state.focused_surface != surface;
		if (!focus_changed) {
			wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		}
	}
}

/* This event is forwarded by the cursor when a pointer emits a
 * _relative_ pointer motion event (i.e. a delta). */
static void
server_cursor_motion(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion *event = data;

	wlr_cursor_move(server->cursor, event->device, event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

/* This event is forwarded by the cursor when a pointer emits an
 * _absolute_ motion event, from 0..1 on each axis. */
static void
server_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;

	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

/* This event is forwarded by the cursor when a pointer emits a button
 * event. */
static void
server_cursor_button(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = data;

	wlr_seat_pointer_notify_button(server->seat,
				       event->time_msec, event->button, event->state);
	if (event->state == WLR_BUTTON_PRESSED) {
		/* Focus that client if the button was pressed. */
		double sx, sy;
		struct wlr_surface *surface;
		struct cg_view *view = desktop_view_at(server,
						       server->cursor->x,
						       server->cursor->y,
						       &surface, &sx, &sy);
		if (view) {
			focus_view(view);
		}
	}
}

/* This event is forwarded by the cursor when a pointer emits an axis
 * event, for example when you move the scroll wheel. */
static void
server_cursor_axis(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;

	wlr_seat_pointer_notify_axis(server->seat,
				     event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source);
}

/* Used to move all of the data necessary to render a surface from the
 * top-level frame handler to the per-surface render function. */
struct render_data {
	struct wlr_output *output;
	struct wlr_renderer *renderer;
	struct cg_view *view;
	struct timespec *when;
};

/* This function is called for every surface that needs to be
   rendered. */
static void
render_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct render_data *rdata = data;
	struct cg_view *view = rdata->view;
	struct wlr_output *output = rdata->output;

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Cannot obtain surface texture");
		return;
	}

	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(view->server->output_layout, output, &ox, &oy);
	ox += view->x + sx, oy += view->y + sy;

	/* We also have to apply the scale factor for HiDPI
	 * outputs. This is only part of the puzzle, Cage does not
	 * fully support HiDPI. */
	struct wlr_box box = {
		.x = ox * output->scale,
		.y = oy * output->scale,
		.width = surface->current.width * output->scale,
		.height = surface->current.height * output->scale,
	};

	float matrix[9];
	enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0, output->transform_matrix);
	wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);
	wlr_surface_send_frame_done(surface, rdata->when);
}

/* This function is called every time an output is ready to display a
 * frame, generally at the output's refresh rate (e.g. 60Hz). */
static void
output_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, frame);
	struct wlr_renderer *renderer = output->server->renderer;

	if (!wlr_output_make_current(output->wlr_output, NULL)) {
		wlr_log(WLR_DEBUG, "Cannot make damage output current");
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);

	wlr_renderer_begin(renderer, width, height);

	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

	struct cg_view *view;
	wl_list_for_each_reverse(view, &output->server->views, link) {
		if (!view->mapped) {
			continue;
		}
		struct render_data rdata = {
			.output = output->wlr_output,
			.view = view,
			.renderer = renderer,
			.when = &now,
		};
		wlr_xdg_surface_for_each_surface(view->xdg_surface, render_surface, &rdata);
	}

	wlr_renderer_end(renderer);
	wlr_output_swap_buffers(output->wlr_output, NULL, NULL);
}

static void
output_destroy_notify(struct wl_listener *listener, void *data)
{
        struct cg_output *output = wl_container_of(listener, output, destroy);
	struct cg_server *server = output->server;

        wl_list_remove(&output->destroy.link);
        wl_list_remove(&output->frame.link);
        free(output);
	server->output = NULL;

	/* Since there is no use in continuing without our (single)
	 * output, terminate. */
	wl_display_terminate(server->wl_display);
}

/* This event is raised by the backend when a new output (aka a
 * display or monitor) becomes available. A kiosk requires only a
 * single output, hence we do nothing in case subsequent outputs
 * become available. */
static void
server_new_output(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* On outputs that have modes, we need to set one before we
	 * can use it.  Each monitor supports only a specific set of
	 * modes. We just pick the last, in the future we could pick
	 * the mode the display advertises as preferred. */
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode = wl_container_of(wlr_output->modes.prev, mode, link);
		wlr_output_set_mode(wlr_output, mode);
	}

	server->output = calloc(1, sizeof(struct cg_output));
	server->output->wlr_output = wlr_output;
	server->output->server = server;
	server->output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &server->output->frame);
	server->output->destroy.notify = output_destroy_notify;
	wl_signal_add(&wlr_output->events.destroy, &server->output->destroy);
	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	/* Disconnect the signal now, because we only use one static output. */
	wl_list_remove(&server->new_output.link);
}

/* Called when the surface is mapped, or ready to display
   on-screen. */
static void
xdg_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, map);
	view->mapped = true;
	
	/* If this is our "root" view, maximize it. Otherwise, center
	   the "child". */
	if (view->xdg_surface->toplevel->parent == NULL) {
		int output_width, output_height;
		struct cg_output *output = view->server->output;
		wlr_output_effective_resolution(output->wlr_output, &output_width, &output_height);
		wlr_xdg_toplevel_set_size(view->xdg_surface, output_width, output_height);
		wlr_xdg_toplevel_set_maximized(view->xdg_surface, true);
	} else {
		center_view(view);	
	}

	focus_view(view);
}

/* Called when the surface is unmapped, and should no longer be
   shown. */
static void
xdg_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
}

/* Called when the surface is destroyed and should never be shown
   again. */
static void
xdg_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_view *view = wl_container_of(listener, view, destroy);
	/* We only listen for events on toplevels, so this is safe. */
	struct wlr_xdg_surface *parent = view->xdg_surface->toplevel->parent;
	struct cg_server *server = view->server;

	wl_list_remove(&view->link);
	free(view);

	/* If this was our "root" toplevel surface, exit. */
	if (parent == NULL /*&& role == WLR_XDG_SURFACE_ROLE_TOPLEVEL FIXME: role is 0?*/) {
		wl_display_terminate(server->wl_display);
	}
}

/* This event is raised when wlr_xdg_shell receives a new xdg surface
 * from a client, either a toplevel (application window) or popup. */
static void
server_new_xdg_surface(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	struct cg_view *view = calloc(1, sizeof(struct cg_view));
	view->server = server;
	view->xdg_surface = xdg_surface;

	view->map.notify = xdg_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	wl_list_insert(&server->views, &view->link);
}

static void
sig_handler(int signal)
{
	wl_display_terminate(server.wl_display);
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("Usage: %s APPLICATION\n", argv[0]);
		return 1;
	}

#ifdef DEBUG
	wlr_log_init(WLR_DEBUG, NULL);
#else
	wlr_log_init(WLR_ERROR, NULL);
#endif

	signal(SIGTERM, sig_handler);
 
	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(server.wl_display, NULL);
	server.renderer = wlr_backend_get_renderer(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	server.output_layout = wlr_output_layout_create();

	wlr_compositor_create(server.wl_display, server.renderer);
	wlr_linux_dmabuf_v1_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	/* Configure a listener to be notified when new outputs are
	 * available on the backend. We use this only to detect the
	 * first output and ignore subsequent outputs. */
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	wl_list_init(&server.views);
	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.wl_display);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&xdg_shell->events.new_surface, &server.new_xdg_surface);

	/* Creates a cursor, which is a wlroots utility for tracking
	 * the cursor image shown on screen. */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which
	 * loads up Xcursor themes to source cursor images from and
	 * makes sure that cursor images are available at all scale
	 * factors on the screen (necessary for HiDPI support). We add
	 * a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/* wlr_cursor *only* displays an image on screen. It does not
	 * move around when the pointer moves. However, we can attach
	 * input devices to it, and it will generate aggregate events
	 * for all of them. In these events, we can choose how we want
	 * to process them, forwarding them to clients and moving the
	 * cursor around. */
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

	/* Configures a seat, which is a single "seat" at which a user
	 * sits and operates the computer. This conceptually includes
	 * up to one keyboard, pointer, touch, and drawing tablet
	 * device. We also rig up a listener to let us know when new
	 * input devices are available on the backend. */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log_errno(WLR_ERROR, "Unable to open Wayland socket");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "Unable to start the wlroots backend");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	if (fork() == 0) {
		execvp(argv[1], (char * const *) argv + 1);
	}

	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
