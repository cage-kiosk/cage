#ifndef CG_XWAYLAND_H
#define CG_XWAYLAND_H

#include <wayland-server-core.h>
#include <wlr/xwayland.h>

#include "view.h"

struct cg_xwayland_view {
	struct cg_view view;
	struct wlr_xwayland_surface *xwayland_surface;

	/* Some applications that aren't yet Wayland-native or
	   otherwise "special" (e.g. Firefox Nightly and Google
	   Chrome/Chromium) spawn an XWayland surface upon startup
	   that is almost immediately closed again. This makes Cage
	   think there are no views left, which results in it
	   exiting. However, after this initial (unmapped) surface,
	   the "real" application surface is opened. This leads to
	   these applications' startup sequences being interrupted by
	   Cage exiting. Hence, to work around this issue, Cage checks
	   whether an XWayland surface has ever been mapped and exits
	   only if 1) the XWayland surface has ever been mapped and 2)
	   this was the last surface Cage manages. */
	bool ever_been_mapped;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	struct wl_listener commit;
	struct wl_listener request_fullscreen;
};

struct cg_xwayland_view *xwayland_view_from_view(struct cg_view *view);
bool xwayland_view_should_manage(struct cg_view *view);
void handle_xwayland_surface_new(struct wl_listener *listener, void *data);

#endif
