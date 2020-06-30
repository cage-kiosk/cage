#ifndef CG_VIEW_H
#define CG_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_surface.h>

#include "output.h"

enum cg_view_type {
	CAGE_XDG_SHELL_VIEW,
};

struct cg_view {
	enum cg_view_type type;
	const struct cg_view_impl *impl;

	struct wl_list link; // cg_output::views
	struct wlr_surface *wlr_surface;
	struct cg_output *output;

	struct {
		/**
		 * Proxy wlr_surface's map signal to the compositor.
		 * Note that cg_view has already taken care of setting up the
		 * view; only the compositor-specific handling such as focus
		 * changing need to be implemented.
		 */
		struct wl_signal map;

		/**
		 * Proxy wlr_surface's unmap signal to the compositor.
		 * Note that cg_view will take care of unmapping the view; only
		 * the compositor-specific handling such as focus changing need
		 * to be implemented.
		 */
		struct wl_signal unmap;
	} events;
};

struct cg_view_impl {
	/**
	 * Get the width and height of a view.
	 */
	void (*get_geometry)(struct cg_view *view, int *width_out, int *height_out);

	/**
	 * Maximize the given view on its output.
	 */
	void (*maximize)(struct cg_view *view, int output_width, int output_height);

	/**
	 * A primary view is a main application window. That is, it is a toplevel window
	 * that has no parent. For example, a dialog window is not a primary view, nor is
	 * e.g. a toolbox window.
	 *
	 * Cage uses this heuristic to decide which views to maximize on their outputs
	 * and which views to display as dialogs, with the caveat that dialogs that extend
	 * their output are maximized regardless.
	 */
	bool (*is_primary)(struct cg_view *view);

	/**
	 * Get a view's title.
	 */
	char *(*get_title)(struct cg_view *view);

	/**
	 * Execute `iterator` on every surface belonging to this view.
	 */
	void (*for_each_surface)(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *user_data);

	/**
	 * Activate a view.
	 */
	void (*activate)(struct cg_view *view, bool activate);
};

bool cage_view_is_primary(struct cg_view *view);
void cage_view_position(struct cg_view *view);
void cage_view_for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *user_data);
char *cage_view_get_title(struct cg_view *view);
void cage_view_activate(struct cg_view *view, bool activate);
bool cage_view_is_mapped(struct cg_view *view);
void cage_view_unmap(struct cg_view *view);
void cage_view_map(struct cg_view *view, struct wlr_surface *surface);
void cage_view_fini(struct cg_view *view);
void cage_view_init(struct cg_view *view, enum cg_view_type type, const struct cg_view_impl *impl,
		    struct cg_output *output);

#endif
