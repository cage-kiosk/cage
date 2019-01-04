#ifndef CG_XWAYLAND_H
#define CG_XWAYLAND_H

#include <wayland-server.h>

void handle_xwayland_surface_new(struct wl_listener *listener, void *data);

#endif
