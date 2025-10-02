/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "config.h"

#include <systemd/sd-daemon.h>

#include "notify.h"

void
notify_set_state(enum cg_notify_state state)
{
	const char *sd_state;

	switch (state) {
	case CAGE_READY:
		sd_state = "READY=1";
		break;
	case CAGE_ALIVE:
		sd_state = "WATCHDOG=1";
		break;
	case CAGE_STOPPING:
		sd_state = "STOPPING=1";
		break;
	}

	sd_notify(0, sd_state);
}
