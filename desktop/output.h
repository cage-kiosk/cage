#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>

struct cg_output {
	struct wlr_output *wlr_output;
	struct wlr_output_damage *damage;

	/**
	 * The views on this output. Ordered from top to bottom.
	 */
	struct wl_list views;

	struct wl_listener mode;
	struct wl_listener transform;
	struct wl_listener destroy;
	struct wl_listener damage_frame;
	struct wl_listener damage_destroy;

	struct wl_list link; // cg_server::outputs
};

void cage_output_get_geometry(struct cg_output *output, struct wlr_box *geometry);
void cage_output_disable(struct cg_output *output);
void cage_output_enable(struct cg_output *output);
void cage_output_init(struct cg_output *output, struct wlr_output *wlr_output);
void cage_output_fini(struct cg_output *output);
void cage_output_set_window_title(struct cg_output *output, const char *title);

#endif
