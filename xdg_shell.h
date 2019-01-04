#ifndef CG_XDG_SHELL_H
#define CG_XDG_SHELL_H

#include <wayland-server.h>

void handle_xdg_shell_surface_new(struct wl_listener *listener, void *data);

#endif
