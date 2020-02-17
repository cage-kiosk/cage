#ifndef CG_VIEW_H
#define CG_VIEW_H

#include "config.h"

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
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
	struct wl_list children; // cg_view_child::link
	struct wlr_surface *wlr_surface;

	/* The view has a position in layout coordinates. */
	int lx, ly;

	enum cg_view_type type;
	const struct cg_view_impl *impl;

	struct wl_listener new_subsurface;
};

struct cg_view_impl {
	char *(*get_title)(struct cg_view *view);
	void (*get_geometry)(struct cg_view *view, int *width_out, int *height_out);
	bool (*is_primary)(struct cg_view *view);
	bool (*is_transient_for)(struct cg_view *child, struct cg_view *parent);
	void (*activate)(struct cg_view *view, bool activate);
	void (*maximize)(struct cg_view *view, int output_width, int output_height);
	void (*destroy)(struct cg_view *view);
	void (*for_each_surface)(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data);
	void (*for_each_popup)(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data);
	struct wlr_surface *(*wlr_surface_at)(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y);
};

struct cg_view_child {
	struct cg_view *view;
	struct wlr_surface *wlr_surface;
	struct wl_list link;

	struct wl_listener commit;
	struct wl_listener new_subsurface;

	void (*destroy)(struct cg_view_child *child);
};

struct cg_subsurface {
	struct cg_view_child view_child;
	struct wlr_subsurface *wlr_subsurface;

	struct wl_listener destroy;
};

char *view_get_title(struct cg_view *view);
bool view_is_primary(struct cg_view *view);
bool view_is_transient_for(struct cg_view *child, struct cg_view *parent);
void view_damage_part(struct cg_view *view);
void view_damage_whole(struct cg_view *view);
void view_activate(struct cg_view *view, bool activate);
void view_position(struct cg_view *view);
void view_for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data);
void view_for_each_popup(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data);
void view_unmap(struct cg_view *view);
void view_map(struct cg_view *view, struct wlr_surface *surface);
void view_destroy(struct cg_view *view);
void view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type, const struct cg_view_impl *impl);

struct cg_view *view_from_wlr_surface(struct cg_server *server, struct wlr_surface *surface);
struct wlr_surface *view_wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y);

void view_child_finish(struct cg_view_child *child);
void view_child_init(struct cg_view_child *child, struct cg_view *view, struct wlr_surface *wlr_surface);

#endif
