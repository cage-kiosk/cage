#ifndef CG_VIEW_H
#define CG_VIEW_H

#include "config.h"

#include <stdbool.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "server.h"

enum cg_view_type {
	CAGE_XDG_SHELL_VIEW,
#ifdef CAGE_HAS_XWAYLAND
	CAGE_XWAYLAND_VIEW,
#endif
};

struct cg_view {
	struct cg_server *server;
	struct wl_list link; // server::views
	struct wlr_surface *wlr_surface;
	int x, y;

	enum cg_view_type type;
	union {
		struct wlr_xdg_surface *xdg_surface;
#ifdef CAGE_HAS_XWAYLAND
		struct wlr_xwayland_surface *xwayland_surface;
#endif
	};

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	// TODO: allow applications to go to fullscreen from maximized?
	// struct wl_listener request_fullscreen;

	void (*activate)(struct cg_view *view, bool activate);
	void (*maximize)(struct cg_view *view, int output_width, int output_height);
	void (*get_geometry)(struct cg_view *view, int *width_out, int *height_out);
	bool (*is_primary)(struct cg_view *view);
};

void view_activate(struct cg_view *view, bool activate);
void view_maximize(struct cg_view *view);
void view_center(struct cg_view *view);
bool view_is_primary(struct cg_view *view);
void view_unmap(struct cg_view *view);
void view_map(struct cg_view *view, struct wlr_surface *surface);
void view_destroy(struct cg_view *view);
struct cg_view *cg_view_create(struct cg_server *server);
struct cg_view *cg_view_from_wlr_surface(struct cg_server *server, struct wlr_surface *surface);

#endif
