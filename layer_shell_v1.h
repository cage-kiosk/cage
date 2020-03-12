#ifndef CG_LAYER_SHELL_V1_H
#define CG_LAYER_SHELL_V1_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

struct cg_layer_surface {
	struct cg_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_node *scene_node;
	struct wl_list link; // cg_output::layers

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener destroy;

	struct wl_listener output_destroy;

	struct wlr_box geometry;
	enum zwlr_layer_shell_v1_layer layer;
};

void handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data);

#endif
