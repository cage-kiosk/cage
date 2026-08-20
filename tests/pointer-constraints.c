/*
 * Cage: A Wayland kiosk.
 *
 * Functional test for pointer-constraint enforcement.
 *
 * Runs as cage's application on the headless backend (see run-test.sh) and
 * drives the compositor from the inside: a zwlr_virtual_pointer_v1 injects
 * input, while a fullscreen toplevel locks its pointer and observes what
 * comes back. Asserts that:
 *
 *   1. the lock activates when the surface gains pointer focus,
 *   2. absolute motion is not delivered as wl_pointer.motion while locked
 *      (the cursor stays pinned), but relative deltas keep flowing,
 *   3. real relative motion reaches the client unmodified while locked,
 *   4. destroying the lock applies the cursor-position hint: a subsequent
 *      relative move lands at hint + delta,
 *   5. shrinking a confined pointer's region below the cursor position warps
 *      the cursor into the new region — reporting the warp as a
 *      wl_pointer.motion, as set_region mandates — instead of wedging the
 *      pointer, and absolute motion aimed outside the region is clamped to its
 *      edge,
 *   6. a confinement whose effective region becomes empty (the constraint
 *      region no longer intersects the input region) is deactivated and lets
 *      the pointer move, and is activated again once the region can hold the
 *      pointer,
 *   7. locking while the cursor is outside the lock's region warps the
 *      pointer into the region, as the protocol guarantees on activation,
 *      while a region committed after the lock activated leaves the pinned
 *      pointer alone,
 *   8. a oneshot lock is deactivated when the pointer focus moves to another
 *      toplevel — applying its cursor-position hint even though the focus is
 *      already elsewhere — and the compositor survives the deactivation
 *      (wlroots destroys a oneshot constraint from inside send_deactivated()),
 *   9. when a centered view moves under a stationary cursor — an output mode
 *      change, applied through zwlr_output_manager_v1, re-centers it — the
 *      seat's surface-local position is resynced and reported as a
 *      wl_pointer.motion, and later confinement clamps run from the new
 *      position rather than the stale one (from which the pointer could
 *      escape the region).
 *
 * Exits 0 on success; cage propagates the exit code when its child exits.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define check(cond, ...)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "FAIL(%d): ", __LINE__);                                                       \
			fprintf(stderr, __VA_ARGS__);                                                                  \
			fprintf(stderr, "\n");                                                                         \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
static struct zwlr_output_manager_v1 *output_manager;

static struct wl_surface *surface;
static struct zwlr_virtual_pointer_v1 *virtual_pointer;
static struct wl_pointer *pointer;
static struct zwp_locked_pointer_v1 *locked_pointer;
static struct zwp_confined_pointer_v1 *confined_pointer;

static bool mapped;
static bool locked;
static bool confined;
static bool have_enter;
static double enter_sx, enter_sy;
static bool have_motion;
static double motion_sx, motion_sy;
static int motion_events;
static int leave_events;
static int rel_events;
static bool have_rel_12_7;

static struct zwlr_output_head_v1 *output_head;
static uint32_t output_manager_serial;
static bool output_manager_done;
static int output_config_result; /* 0 pending, 1 succeeded, -1 failed or cancelled */

/* The head's per-output events (name, modes, ...) are of no use here — only
 * the head object itself is, to enable it with a custom mode — so no listener
 * is attached to it. */
static void
output_manager_handle_head(void *data, struct zwlr_output_manager_v1 *manager, struct zwlr_output_head_v1 *head)
{
	check(output_head == NULL, "expected a single output head");
	output_head = head;
}

static void
output_manager_handle_done(void *data, struct zwlr_output_manager_v1 *manager, uint32_t serial)
{
	output_manager_serial = serial;
	output_manager_done = true;
}

static void
output_manager_handle_finished(void *data, struct zwlr_output_manager_v1 *manager)
{
}

static const struct zwlr_output_manager_v1_listener output_manager_listener = {
	.head = output_manager_handle_head,
	.done = output_manager_handle_done,
	.finished = output_manager_handle_finished,
};

static void
output_config_succeeded(void *data, struct zwlr_output_configuration_v1 *config)
{
	output_config_result = 1;
}

static void
output_config_failed(void *data, struct zwlr_output_configuration_v1 *config)
{
	output_config_result = -1;
}

static void
output_config_cancelled(void *data, struct zwlr_output_configuration_v1 *config)
{
	output_config_result = -1;
}

static const struct zwlr_output_configuration_v1_listener output_config_listener = {
	.succeeded = output_config_succeeded,
	.failed = output_config_failed,
	.cancelled = output_config_cancelled,
};

// ---- registry ----

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
		constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
	} else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
		relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		virtual_pointer_manager =
			wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		output_manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, 1);
		zwlr_output_manager_v1_add_listener(output_manager, &output_manager_listener, NULL);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

// ---- xdg-shell ----

static void
wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static int32_t configured_width, configured_height;

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h, struct wl_array *states)
{
	configured_width = w;
	configured_height = h;
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	check(false, "toplevel closed by the compositor");
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static struct wl_buffer *
make_buffer(int32_t width, int32_t height)
{
	char template[] = "/tmp/cage-test-shm-XXXXXX";
	int stride = width * 4;
	int size = stride * height;
	int fd = mkstemp(template);
	check(fd >= 0, "cannot create shm file");
	unlink(template);
	check(ftruncate(fd, size) == 0, "cannot truncate shm file");

	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(pixels != MAP_FAILED, "cannot map shm buffer");
	memset(pixels, 0x77, size);
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!mapped) {
		mapped = true;
		/* Commit at the configured size, so the (maximized) surface
		 * covers the whole output and the cursor is always over it. */
		int32_t width = configured_width > 0 ? configured_width : 640;
		int32_t height = configured_height > 0 ? configured_height : 480;
		wl_surface_attach(surface, make_buffer(width, height), 0, 0);
		wl_surface_commit(surface);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

/* Second toplevel, mapped later to take the pointer focus away. */
static struct wl_surface *surface2;
static bool mapped2;

static void
xdg_surface2_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!mapped2) {
		mapped2 = true;
		int32_t width = configured_width > 0 ? configured_width : 640;
		int32_t height = configured_height > 0 ? configured_height : 480;
		wl_surface_attach(surface2, make_buffer(width, height), 0, 0);
		wl_surface_commit(surface2);
	}
}

static const struct xdg_surface_listener xdg_surface2_listener = {
	.configure = xdg_surface2_handle_configure,
};

/* Third toplevel: a small dialog with an xdg_toplevel parent, so cage centers
 * it instead of maximizing it — the view that moves when the output shrinks. */
#define DIALOG_SIZE 200

static struct wl_surface *surface3;
static bool mapped3;

static void
xdg_surface3_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!mapped3) {
		mapped3 = true;
		wl_surface_attach(surface3, make_buffer(DIALOG_SIZE, DIALOG_SIZE), 0, 0);
		wl_surface_commit(surface3);
	}
}

static const struct xdg_surface_listener xdg_surface3_listener = {
	.configure = xdg_surface3_handle_configure,
};

// ---- pointer + constraint events ----

static void
locked_handle_locked(void *data, struct zwp_locked_pointer_v1 *lp)
{
	locked = true;
}

static void
locked_handle_unlocked(void *data, struct zwp_locked_pointer_v1 *lp)
{
	locked = false;
}

static const struct zwp_locked_pointer_v1_listener locked_listener = {
	.locked = locked_handle_locked,
	.unlocked = locked_handle_unlocked,
};

static void
confined_handle_confined(void *data, struct zwp_confined_pointer_v1 *cp)
{
	confined = true;
}

static void
confined_handle_unconfined(void *data, struct zwp_confined_pointer_v1 *cp)
{
	confined = false;
}

static const struct zwp_confined_pointer_v1_listener confined_listener = {
	.confined = confined_handle_confined,
	.unconfined = confined_handle_unconfined,
};

static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy)
{
	have_enter = true;
	enter_sx = wl_fixed_to_double(sx);
	enter_sy = wl_fixed_to_double(sy);
}

static void
pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	leave_events++;
}

static void
pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	have_motion = true;
	motion_sx = wl_fixed_to_double(sx);
	motion_sy = wl_fixed_to_double(sy);
	motion_events++;
}

static void
pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
}

static void
pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
pointer_frame(void *data, struct wl_pointer *p)
{
}

static void
pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void
relative_motion(void *data, struct zwp_relative_pointer_v1 *rp, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
		wl_fixed_t udx, wl_fixed_t udy)
{
	rel_events++;
	if (fabs(wl_fixed_to_double(dx) - 12.0) < 0.51 && fabs(wl_fixed_to_double(dy) - 7.0) < 0.51) {
		have_rel_12_7 = true;
	}
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {
	.relative_motion = relative_motion,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t capabilities)
{
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);

		struct zwp_relative_pointer_v1 *relative =
			zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
		zwp_relative_pointer_v1_add_listener(relative, &relative_listener, NULL);

		/* Lock before the surface has pointer focus: activation must
		 * follow from the focus change. */
		locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
			constraints, surface, pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(locked_pointer, &locked_listener, NULL);
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

// ---- test driver ----

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Dispatch events for up to timeout_ms, or until an event batch arrives. */
static void
pump(int timeout_ms)
{
	wl_display_flush(display);
	struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
	if (poll(&pfd, 1, timeout_ms) > 0) {
		check(wl_display_dispatch(display) != -1, "display connection lost");
	} else {
		wl_display_dispatch_pending(display);
	}
}

static void
inject_absolute(uint32_t x, uint32_t y)
{
	zwlr_virtual_pointer_v1_motion_absolute(virtual_pointer, 0, x, y, 10000, 10000);
	zwlr_virtual_pointer_v1_frame(virtual_pointer);
}

static void
inject_relative(double dx, double dy)
{
	zwlr_virtual_pointer_v1_motion(virtual_pointer, 0, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(virtual_pointer);
}

int
main(void)
{
	/* Backstop: die loudly rather than hang the test runner (a roundtrip
	 * against a deadlocked compositor blocks forever). Only failing steps
	 * wait their deadlines out — a passing run takes milliseconds — so
	 * this must merely exceed the ~55s worst-case sum of the deadlines
	 * below, and stay under the 90s meson timeout. */
	alarm(70);

	display = wl_display_connect(NULL);
	check(display != NULL, "no wayland display");
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	check(compositor && shm && seat && wm_base, "missing core globals");
	check(constraints != NULL, "zwp_pointer_constraints_v1 not advertised");
	check(relative_manager != NULL, "zwp_relative_pointer_manager_v1 not advertised");
	check(virtual_pointer_manager != NULL, "zwlr_virtual_pointer_manager_v1 not advertised");
	check(output_manager != NULL, "zwlr_output_manager_v1 not advertised");

	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	wl_seat_add_listener(seat, &seat_listener, NULL);

	/* The virtual pointer gives the seat its pointer capability; the
	 * capability event then creates the wl_pointer and the lock. */
	virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(virtual_pointer_manager, NULL);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "pointer-constraints-test");
	wl_surface_commit(surface);

	/* 1. Move the pointer onto the surface: the lock must activate. The
	 * injection is repeated (with jitter, so every send is a real motion
	 * event) because it races the surface appearing on the compositor
	 * side. */
	int64_t deadline = now_ms() + 10000;
	uint32_t jitter = 0;
	while (!(mapped && locked_pointer && locked) && now_ms() < deadline) {
		if (mapped && locked_pointer) {
			inject_absolute(5000 + (jitter++ % 2) * 100, 5000);
		}
		pump(100);
	}
	check(locked, "pointer lock did not activate on focus");

	/* 2. Absolute motion while locked: the cursor stays pinned (no
	 * wl_pointer.motion or leave), but relative deltas are synthesized. */
	motion_events = 0;
	leave_events = 0;
	rel_events = 0;
	inject_absolute(2000, 2000);
	inject_absolute(8000, 8000);
	inject_absolute(3000, 7000);
	/* 3. Real relative motion flows while locked, unmodified. */
	inject_relative(12.0, 7.0);
	wl_display_roundtrip(display);
	deadline = now_ms() + 2000;
	while ((rel_events < 4 || !have_rel_12_7) && now_ms() < deadline) {
		pump(100);
	}
	check(motion_events == 0, "absolute motion leaked through the lock (%d motion events)", motion_events);
	check(leave_events == 0, "pointer left the surface while locked");
	check(rel_events >= 4, "expected relative deltas while locked, got %d", rel_events);
	check(have_rel_12_7, "relative delta was modified by the lock");

	/* 4. Destroy the lock with a cursor-position hint: the cursor must
	 * reappear at the hint, so a relative move of (25, 30) from the hint
	 * (100, 50) lands at exactly (125, 80). */
	zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer, wl_fixed_from_int(100), wl_fixed_from_int(50));
	wl_surface_commit(surface);
	zwp_locked_pointer_v1_destroy(locked_pointer);
	wl_display_roundtrip(display);

	have_motion = false;
	inject_relative(25.0, 30.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the lock was destroyed");
	check(fabs(motion_sx - 125.0) < 1.0 && fabs(motion_sy - 80.0) < 1.0,
	      "cursor-position hint not applied: motion at %.2f,%.2f, expected 125,80", motion_sx, motion_sy);

	/* 5. Confine the pointer (whole surface, so the cursor starts inside),
	 * then shrink the region to a rectangle that excludes the cursor. The
	 * compositor must warp the cursor into the new region — to its nearest
	 * point, half a pixel inside the top-left corner at (300.5, 300.5) —
	 * or every later relative motion would be clamped to zero and the
	 * pointer would be wedged. set_region mandates that the warp be
	 * reported: "If warped, a wl_pointer.motion event will be emitted." */
	confined_pointer = zwp_pointer_constraints_v1_confine_pointer(constraints, surface, pointer, NULL,
								      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_confined_pointer_v1_add_listener(confined_pointer, &confined_listener, NULL);
	deadline = now_ms() + 2000;
	while (!confined && now_ms() < deadline) {
		pump(100);
	}
	check(confined, "pointer confinement did not activate");

	struct wl_region *region = wl_compositor_create_region(compositor);
	wl_region_add(region, 300, 300, 100, 100);
	have_motion = false;
	zwp_confined_pointer_v1_set_region(confined_pointer, region);
	wl_region_destroy(region);
	wl_surface_commit(surface);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "the set_region warp into the shrunken region sent no wl_pointer.motion");
	check(fabs(motion_sx - 300.5) < 1.0 && fabs(motion_sy - 300.5) < 1.0,
	      "cursor not warped into the shrunken confine region: motion at %.2f,%.2f, expected 300.5,300.5",
	      motion_sx, motion_sy);

	/* And the pointer still moves from there, rather than being wedged
	 * outside the region with every delta clamped to zero. */
	have_motion = false;
	inject_relative(5.0, 5.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the confine region shrank: pointer is wedged");
	check(fabs(motion_sx - 305.5) < 1.0 && fabs(motion_sy - 305.5) < 1.0,
	      "motion after the set_region warp did not start from the region: motion at %.2f,%.2f, expected "
	      "305.5,305.5",
	      motion_sx, motion_sy);

	/* Absolute motion aimed outside the region is clamped to its edge, not
	 * dropped: from (305.5, 305.5), a target of (0, 0) slides to the
	 * region's top-left corner (300, 300). */
	have_motion = false;
	inject_absolute(0, 0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "absolute motion outside the confine region was dropped instead of clamped");
	check(fabs(motion_sx - 300.0) < 1.0 && fabs(motion_sy - 300.0) < 1.0,
	      "absolute motion not clamped to the confine region: motion at %.2f,%.2f, expected 300,300", motion_sx,
	      motion_sy);

	/* 6. Make the effective confine region empty: a constraint region that
	 * does not intersect the surface's input region. The protocol
	 * guarantees the pointer is inside the region whenever the confinement
	 * is active, which an empty region cannot satisfy, so the compositor
	 * must deactivate the confinement — and then let the pointer move,
	 * rather than freezing the cursor for as long as the constraint
	 * lives. */
	struct wl_region *input_region = wl_compositor_create_region(compositor);
	wl_region_add(input_region, 0, 0, 400, 400);
	wl_surface_set_input_region(surface, input_region);
	wl_region_destroy(input_region);
	region = wl_compositor_create_region(compositor);
	wl_region_add(region, 600, 600, 50, 50);
	zwp_confined_pointer_v1_set_region(confined_pointer, region);
	wl_region_destroy(region);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	deadline = now_ms() + 2000;
	while (confined && now_ms() < deadline) {
		pump(100);
	}
	check(!confined, "confinement not deactivated when its effective region became empty");

	double base_sx = motion_sx;
	double base_sy = motion_sy;
	have_motion = false;
	inject_relative(10.0, 10.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion with an empty confine region: pointer is wedged");
	check(fabs(motion_sx - (base_sx + 10.0)) < 1.0 && fabs(motion_sy - (base_sy + 10.0)) < 1.0,
	      "motion with an empty confine region was clamped: motion at %.2f,%.2f, expected %.2f,%.2f", motion_sx,
	      motion_sy, base_sx + 10.0, base_sy + 10.0);

	/* Restoring the input region makes the effective region — the (600,
	 * 600) rectangle set above — non-empty again: the confinement must
	 * activate on that commit, warping the pointer into the region to its
	 * nearest point (600.5, 600.5). The activation warp is not a
	 * set_region warp and sends no motion event, so observe it through the
	 * next relative move. */
	wl_surface_set_input_region(surface, NULL);
	wl_surface_commit(surface);
	deadline = now_ms() + 2000;
	while (!confined && now_ms() < deadline) {
		pump(100);
	}
	check(confined, "confinement not activated again once its region could hold the pointer");

	have_motion = false;
	inject_relative(5.0, 5.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the confinement activated again");
	check(fabs(motion_sx - 605.5) < 1.0 && fabs(motion_sy - 605.5) < 1.0,
	      "pointer not warped into the region when the confinement activated again: motion at %.2f,%.2f, expected "
	      "605.5,605.5",
	      motion_sx, motion_sy);

	zwp_confined_pointer_v1_destroy(confined_pointer);
	wl_display_roundtrip(display);

	/* 7. Lock with a region that excludes the current cursor position (605.5,
	 * 605.5): the protocol guarantees the pointer is inside the region
	 * whenever the lock activates, so the compositor must warp it into the
	 * region — to its nearest point, (549.5, 549.5) — before pinning. The
	 * warp sends no motion event; observe it through a relative move after
	 * unlocking without a hint, which leaves the cursor where it was
	 * pinned. */
	locked = false;
	region = wl_compositor_create_region(compositor);
	wl_region_add(region, 500, 500, 50, 50);
	locked_pointer = zwp_pointer_constraints_v1_lock_pointer(constraints, surface, pointer, region,
								 ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	wl_region_destroy(region);
	zwp_locked_pointer_v1_add_listener(locked_pointer, &locked_listener, NULL);
	deadline = now_ms() + 2000;
	while (!locked && now_ms() < deadline) {
		pump(100);
	}
	check(locked, "region-restricted pointer lock did not activate");

	/* A lock's region is only the gate its activation passes through — "the
	 * region where the pointer must be in order for the lock to activate" —
	 * so committing a region that excludes the pinned position must neither
	 * warp the pointer nor break the lock. */
	motion_events = 0;
	region = wl_compositor_create_region(compositor);
	wl_region_add(region, 100, 100, 20, 20);
	zwp_locked_pointer_v1_set_region(locked_pointer, region);
	wl_region_destroy(region);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);
	check(locked, "a region excluding the pinned position broke the lock");
	check(motion_events == 0, "a locked pointer was sent motion by set_region (%d motion events)", motion_events);

	zwp_locked_pointer_v1_destroy(locked_pointer);
	wl_display_roundtrip(display);

	have_motion = false;
	inject_relative(5.0, 5.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the region-restricted lock was destroyed");
	check(fabs(motion_sx - 554.5) < 1.0 && fabs(motion_sy - 554.5) < 1.0,
	      "lock did not pin the pointer at the point it warped to on activation: motion at %.2f,%.2f, expected "
	      "554.5,554.5",
	      motion_sx, motion_sy);

	/* 8. A oneshot lock is deactivated when the pointer focus moves away —
	 * here, to a second toplevel mapped on top. wlroots destroys a oneshot
	 * constraint from inside send_deactivated(), so this exercises the
	 * compositor's reentrant teardown path. The lock also carries a
	 * cursor-position hint, which must be applied even though the pointer
	 * focus has already moved on by the time the lock is broken. */
	locked = false;
	locked_pointer = zwp_pointer_constraints_v1_lock_pointer(constraints, surface, pointer, NULL,
								 ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	zwp_locked_pointer_v1_add_listener(locked_pointer, &locked_listener, NULL);
	zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer, wl_fixed_from_int(420), wl_fixed_from_int(210));
	wl_surface_commit(surface);
	deadline = now_ms() + 2000;
	while (!locked && now_ms() < deadline) {
		pump(100);
	}
	check(locked, "oneshot pointer lock did not activate");

	surface2 = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface2 = xdg_wm_base_get_xdg_surface(wm_base, surface2);
	xdg_surface_add_listener(xdg_surface2, &xdg_surface2_listener, NULL);
	struct xdg_toplevel *toplevel2 = xdg_surface_get_toplevel(xdg_surface2);
	xdg_toplevel_add_listener(toplevel2, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel2, "pointer-constraints-test-2");
	wl_surface_commit(surface2);

	deadline = now_ms() + 5000;
	while (locked && now_ms() < deadline) {
		pump(100);
	}
	check(!locked, "oneshot lock not deactivated when the focus moved to another toplevel");

	/* The hint must have been applied on deactivation, so a relative move
	 * of (10, 10) from the hint (420, 210) lands at (430, 220) — observed
	 * on the second toplevel, which occupies the same layout space. */
	wl_display_roundtrip(display);
	have_motion = false;
	inject_relative(10.0, 10.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the oneshot lock was broken");
	check(fabs(motion_sx - 430.0) < 1.0 && fabs(motion_sy - 220.0) < 1.0,
	      "cursor-position hint not applied on focus-change deactivation: motion at %.2f,%.2f, expected 430,220",
	      motion_sx, motion_sy);

	/* The compositor already destroyed its side; drop the defunct resource
	 * and the second toplevel, and verify the compositor survived. */
	zwp_locked_pointer_v1_destroy(locked_pointer);
	xdg_toplevel_destroy(toplevel2);
	xdg_surface_destroy(xdg_surface2);
	wl_surface_destroy(surface2);
	check(wl_display_roundtrip(display) != -1, "compositor died deactivating the oneshot lock");

	/* 9. A view that moves under a stationary cursor. Map a small dialog
	 * (an xdg_toplevel with a parent, which cage centers rather than
	 * maximizes), confine the pointer on it, then shrink the output mode:
	 * the dialog is re-centered, changing the cursor's surface-local
	 * position without moving the cursor. The compositor must resync the
	 * seat's surface-local position and report it as a wl_pointer.motion;
	 * a stale position would misdirect every later confinement clamp and
	 * let the pointer escape the region. The first toplevel's maximized
	 * size is the output size — the base for all expected coordinates. */
	int32_t output_width = configured_width;
	int32_t output_height = configured_height;
	check(output_width > 400 && output_height > 240, "output too small for the mode-change scenario (%dx%d)",
	      output_width, output_height);

	surface3 = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface3 = xdg_wm_base_get_xdg_surface(wm_base, surface3);
	xdg_surface_add_listener(xdg_surface3, &xdg_surface3_listener, NULL);
	struct xdg_toplevel *toplevel3 = xdg_surface_get_toplevel(xdg_surface3);
	xdg_toplevel_add_listener(toplevel3, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel3, "pointer-constraints-test-dialog");
	xdg_toplevel_set_parent(toplevel3, toplevel);
	wl_surface_commit(surface3);

	deadline = now_ms() + 5000;
	while (!mapped3 && now_ms() < deadline) {
		pump(100);
	}
	check(mapped3, "the dialog toplevel was not configured");
	/* Make sure the compositor has processed the mapping commit before
	 * aiming the cursor at the centered dialog. */
	wl_display_roundtrip(display);

	/* Move the cursor to the output's center: the dialog's center, at
	 * surface-local (DIALOG_SIZE / 2, DIALOG_SIZE / 2). */
	have_enter = false;
	inject_absolute(5000, 5000);
	deadline = now_ms() + 2000;
	while (!have_enter && now_ms() < deadline) {
		pump(100);
	}
	check(have_enter, "cursor did not enter the centered dialog");
	check(fabs(enter_sx - DIALOG_SIZE / 2) < 1.0 && fabs(enter_sy - DIALOG_SIZE / 2) < 1.0,
	      "cursor not at the dialog's center: enter at %.2f,%.2f, expected %d,%d", enter_sx, enter_sy,
	      DIALOG_SIZE / 2, DIALOG_SIZE / 2);

	/* Confine the pointer to a region slightly smaller than the dialog, so
	 * clamping to the region's edge is observable strictly inside the
	 * surface. */
	confined = false;
	region = wl_compositor_create_region(compositor);
	wl_region_add(region, 0, 0, DIALOG_SIZE - 10, DIALOG_SIZE - 10);
	confined_pointer = zwp_pointer_constraints_v1_confine_pointer(constraints, surface3, pointer, region,
								      ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	wl_region_destroy(region);
	zwp_confined_pointer_v1_add_listener(confined_pointer, &confined_listener, NULL);
	deadline = now_ms() + 2000;
	while (!confined && now_ms() < deadline) {
		pump(100);
	}
	check(confined, "pointer confinement on the dialog did not activate");

	/* Shrink the output by (160, 80): the re-centered dialog moves by
	 * (-80, -40), so the stationary cursor's surface-local position moves
	 * from the center by (+80, +40) — still inside the confine region, so
	 * no warp is involved. Drain pending zwlr_output_manager_v1.done
	 * events first; a stale serial would cancel the configuration. */
	wl_display_roundtrip(display);
	check(output_head != NULL && output_manager_done, "no zwlr_output_manager_v1 head advertised");
	struct zwlr_output_configuration_v1 *output_config =
		zwlr_output_manager_v1_create_configuration(output_manager, output_manager_serial);
	zwlr_output_configuration_v1_add_listener(output_config, &output_config_listener, NULL);
	struct zwlr_output_configuration_head_v1 *config_head =
		zwlr_output_configuration_v1_enable_head(output_config, output_head);
	zwlr_output_configuration_head_v1_set_custom_mode(config_head, output_width - 160, output_height - 80, 0);
	have_motion = false;
	zwlr_output_configuration_v1_apply(output_config);
	deadline = now_ms() + 5000;
	while (output_config_result == 0 && now_ms() < deadline) {
		pump(100);
	}
	check(output_config_result == 1, "the output mode change was not applied (%d)", output_config_result);
	zwlr_output_configuration_v1_destroy(output_config);

	/* The resync must arrive as a motion event, with no input injected. */
	double moved_sx = DIALOG_SIZE / 2 + 80;
	double moved_sy = DIALOG_SIZE / 2 + 40;
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after the dialog was re-centered under the stationary cursor");
	check(fabs(motion_sx - moved_sx) < 1.0 && fabs(motion_sy - moved_sy) < 1.0,
	      "stale surface-local position after the dialog moved: motion at %.2f,%.2f, expected %.0f,%.0f", motion_sx,
	      motion_sy, moved_sx, moved_sy);

	/* And confinement clamps from the resynced position: a motion aiming
	 * past the region's right edge stops at its last row of pixels,
	 * (DIALOG_SIZE - 11) — from the stale position the same delta would
	 * have sailed through, taking the pointer out of the region. */
	have_motion = false;
	inject_relative(50.0, 0.0);
	deadline = now_ms() + 2000;
	while (!have_motion && now_ms() < deadline) {
		pump(100);
	}
	check(have_motion, "no motion after a clamped move inside the confine region");
	check(fabs(motion_sx - (DIALOG_SIZE - 11)) < 1.0 && fabs(motion_sy - moved_sy) < 1.0,
	      "confinement clamp did not run from the resynced position: motion at %.2f,%.2f, expected %d,%.0f",
	      motion_sx, motion_sy, DIALOG_SIZE - 11, moved_sy);

	zwp_confined_pointer_v1_destroy(confined_pointer);
	xdg_toplevel_destroy(toplevel3);
	xdg_surface_destroy(xdg_surface3);
	wl_surface_destroy(surface3);
	check(wl_display_roundtrip(display) != -1, "compositor died after the mode-change scenario");

	printf("pointer-constraints test passed\n");
	return 0;
}
