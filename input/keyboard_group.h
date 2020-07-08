#ifndef CG_KEYBOARD_GROUP_H
#define CG_KEYBOARD_GROUP_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_seat.h>

struct cg_keyboard_group {
	struct wlr_keyboard_group *wlr_keyboard_group;
	struct wlr_seat *wlr_seat;

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_list link; // seat::keyboard_groups
};

bool cage_keyboard_group_add(struct cg_keyboard_group *group, struct wlr_input_device *wlr_input_device);
bool cage_keyboard_group_init(struct cg_keyboard_group *group, struct wlr_input_device *wlr_input_device,
			      struct wlr_seat *wlr_seat);
void cage_keyboard_group_fini(struct cg_keyboard_group *group);

#endif
