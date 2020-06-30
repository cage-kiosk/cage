#ifndef CG_XDG_SHELL_H
#define CG_XDG_SHELL_H

#include <wlr/types/wlr_xdg_shell.h>

#include "output.h"
#include "view.h"

struct cg_xdg_shell_view {
	struct cg_view view;
	struct wlr_xdg_surface *xdg_surface;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_fullscreen;
};

void cage_xdg_shell_view_init(struct cg_xdg_shell_view *xdg_shell_view, struct wlr_xdg_surface *wlr_xdg_surface,
			      struct cg_output *output);

#endif
