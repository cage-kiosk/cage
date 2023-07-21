#ifndef CG_VIEW_H
#define CG_VIEW_H

#include "config.h"

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "server.h"

enum cg_view_type {
	CAGE_XDG_SHELL_VIEW,
#if CAGE_HAS_XWAYLAND
	CAGE_XWAYLAND_VIEW,
#endif
};

struct cg_view {
	struct cg_server *server;
	struct wl_list link; // server::views
	struct wlr_surface *wlr_surface;
	struct wlr_scene_tree *scene_tree;

	/* The view has a position in layout coordinates. */
	int lx, ly;

	enum cg_view_type type;
	const struct cg_view_impl *impl;
};

struct cg_view_impl {
	char *(*get_title)(struct cg_view *view);
	void (*get_geometry)(struct cg_view *view, int *width_out, int *height_out);
	bool (*is_primary)(struct cg_view *view);
	bool (*is_transient_for)(struct cg_view *child, struct cg_view *parent);
	void (*activate)(struct cg_view *view, bool activate);
	void (*maximize)(struct cg_view *view, int output_width, int output_height);
	void (*destroy)(struct cg_view *view);
};

char *view_get_title(struct cg_view *view);
bool view_is_primary(struct cg_view *view);
bool view_is_transient_for(struct cg_view *child, struct cg_view *parent);
void view_activate(struct cg_view *view, bool activate);
void view_position(struct cg_view *view);
void view_position_all(struct cg_server *server);
void view_unmap(struct cg_view *view);
void view_map(struct cg_view *view, struct wlr_surface *surface);
void view_destroy(struct cg_view *view);
void view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type, const struct cg_view_impl *impl);

struct cg_view *view_from_wlr_surface(struct wlr_surface *surface);

#endif
