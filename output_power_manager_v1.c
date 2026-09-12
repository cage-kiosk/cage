#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>

#include "output.h"
#include "output_power_manager_v1.h"
#include "server.h"

void
handle_output_power_manager_set_mode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct cg_output *output = event->output->data;

	if (!output) {
		return;
	}

	output_set_power(output, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}
