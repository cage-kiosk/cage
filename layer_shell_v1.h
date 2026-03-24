#ifndef CG_LAYER_SHELL_V1_H
#define CG_LAYER_SHELL_V1_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

struct cg_output;

struct cg_layer_surface {
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener node_destroy;
	struct wl_listener new_popup;

	bool mapped;

	struct wlr_scene_tree *popups;

	struct cg_output *output;
	struct wl_list link; // cg_output::layer_surfaces

	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_layer_surface_v1 *layer_surface;
};

struct cg_layer_popup {
	struct wlr_xdg_popup *wlr_popup;
	struct wlr_scene_tree *scene;
	struct cg_layer_surface *toplevel;

	struct wl_listener destroy;
	struct wl_listener new_popup;
	struct wl_listener commit;
};

void handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data);

#endif
