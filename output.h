#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server.h>
#include <wlr/types/wlr_output.h>

#include "server.h"

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;

	struct wl_listener frame;
	struct wl_listener mode;
	struct wl_listener transform;
	struct wl_listener destroy;
};

void handle_new_output(struct wl_listener *listener, void *data);

#endif
