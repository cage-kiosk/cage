#ifndef CG_FULLSCREEN_SHELL_H
#define CG_FULLSCREEN_SHELL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_fullscreen_shell_v1.h>

#include "view.h"

struct cg_fullscreen_shell_view {
	struct cg_view view;
};

void handle_fullscreen_shell_present_surface(struct wl_listener *listener, void *data);

#endif
