#ifndef CG_POINTER_H
#define CG_POINTER_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>

struct cg_pointer {
	struct wl_list link; // seat::pointers
	struct wlr_input_device *device;

	struct wl_listener destroy;
};

void cage_pointer_init(struct cg_pointer *pointer, struct wlr_input_device *device);
void cage_pointer_fini(struct cg_pointer *pointer);

#endif
