#ifndef CG_NOTIFY_H
#define CG_NOTIFY_H

#include "config.h"

#define CAGE_ALIVE_PERIOD_MS (20000)

enum cg_notify_state {
	CAGE_READY,
	CAGE_ALIVE,
	CAGE_STOPPING,
};

#if !CAGE_HAS_SYSTEMD
static inline void
notify_set_state(enum cg_notify_state state)
{
	/* Nothing */
}
#else
void notify_set_state(enum cg_notify_state state);
#endif

#endif
