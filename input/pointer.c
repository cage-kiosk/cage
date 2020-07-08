/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 * Copyright (C) 2019 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_device.h>

#include "pointer.h"

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct cg_pointer *pointer = wl_container_of(listener, pointer, destroy);
	cage_pointer_fini(pointer);
}

void
cage_pointer_init(struct cg_pointer *pointer, struct wlr_input_device *device)
{
	assert(pointer != NULL);
	assert(device != NULL);

	pointer->device = device;
	pointer->destroy.notify = handle_pointer_destroy;
	wl_signal_add(&device->events.destroy, &pointer->destroy);
}

void
cage_pointer_fini(struct cg_pointer *pointer)
{
	assert(pointer != NULL);

	wl_list_remove(&pointer->link);
	wl_list_remove(&pointer->destroy.link);

	free(pointer);
}
