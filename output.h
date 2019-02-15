#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>

#include "seat.h"
#include "server.h"
#include "view.h"

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;
	struct wlr_output_damage *damage;

	struct wl_listener mode;
	struct wl_listener transform;
	struct wl_listener destroy;
	struct wl_listener damage_frame;
	struct wl_listener damage_destroy;
};

void handle_new_output(struct wl_listener *listener, void *data);
void output_damage_view_surface(struct cg_output *output, struct cg_view *view);
void output_damage_view_whole(struct cg_output *cg_output, struct cg_view *view);
void output_damage_drag_icon(struct cg_output *output, struct cg_drag_icon *icon);
void output_set_window_title(struct cg_output *output, const char *title);

#endif
