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
	struct wl_listener commit;
	// TODO: allow applications to go to fullscreen from maximized?
	// struct wl_listener request_fullscreen;

	struct wl_listener new_popup;
};

struct cg_xdg_popup {
	struct cg_view_child view_child;
	struct wlr_xdg_popup *wlr_popup;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener new_popup;
};

void handle_xdg_shell_surface_new(struct wl_listener *listener, void *data);

#endif
