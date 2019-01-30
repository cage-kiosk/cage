#ifndef CG_XDG_SHELL_H
#define CG_XDG_SHELL_H

#include <wayland-server.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "view.h"

struct cg_xdg_shell_view {
	struct cg_view view;
	struct wlr_xdg_surface *xdg_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	// TODO: allow applications to go to fullscreen from maximized?
	// struct wl_listener request_fullscreen;
};

void handle_xdg_shell_surface_new(struct wl_listener *listener, void *data);

#endif
