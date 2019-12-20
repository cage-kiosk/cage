#ifndef CG_XDG_SHELL_H
#define CG_XDG_SHELL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "view.h"

struct cg_xdg_shell_view {
	struct cg_view view;
	struct wlr_xdg_surface *xdg_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	struct wl_listener commit;
	struct wl_listener request_fullscreen;
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

struct cg_xdg_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
	struct cg_server *server;
	struct wl_listener destroy;
	struct wl_listener request_mode;
};

void handle_xdg_shell_surface_new(struct wl_listener *listener, void *data);

void handle_xdg_toplevel_decoration(struct wl_listener *listener, void *data);

#endif
