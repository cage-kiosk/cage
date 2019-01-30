#ifndef CG_XWAYLAND_H
#define CG_XWAYLAND_H

#include <wayland-server.h>
#include <wlr/xwayland.h>

#include "view.h"

struct cg_xwayland_view {
	struct cg_view view;
	struct wlr_xwayland_surface *xwayland_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	// TODO: allow applications to go to fullscreen from maximized?
	// struct wl_listener request_fullscreen;
};

struct cg_xwayland_view *xwayland_view_from_view(struct cg_view *view);
void handle_xwayland_surface_new(struct wl_listener *listener, void *data);

#endif
