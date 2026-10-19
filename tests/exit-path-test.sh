#!/bin/bash
# Fault-injection tests for cage's startup error paths.
#
# main() registers listeners on wlroots signal objects but removes them
# in a cleanup block that only the normal exit path reaches: every
# startup failure does "goto end" and skips it, so wlroots' destroy-time
# assertions (e.g. types/wlr_output_layout.c, which asserts that the
# layout's change-signal has no listeners left) abort the process with
# SIGABRT instead of letting cage exit cleanly with status 1.
#
# These tests force startup to fail at two different points:
#
#   1. backend creation fails - the first possible failure, before any
#      listener is registered. Guards against broken fixes that remove
#      listeners unconditionally: the listener nodes are zero-initialized,
#      so wl_list_remove() on a never-registered node dereferences NULL.
#
#   2. Wayland socket creation fails - the last possible failure before
#      wlr_backend_start(), with every listener registered. This is the
#      original bug report: a dirty XDG_RUNTIME_DIR makes
#      wl_display_add_socket_auto() return NULL.
#
# In both cases cage must exit with status 1 and must not die on an
# assertion. Socket failure is injected by pointing XDG_RUNTIME_DIR at
# a regular file: binding $XDG_RUNTIME_DIR/wayland-N then fails with
# ENOTDIR for every name, for any uid (root-proof, container-proof).
#
# Usage: tests/exit-path-test.sh [path-to-cage]  (default: build/cage)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CAGE="${1:-$ROOT/build/cage}"

[ -x "$CAGE" ] || { echo "cage binary not found: $CAGE" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ulimit -c 0 2>/dev/null || true

fail=0

# run_case <name> <expected-log-message> [VAR=value...]
run_case() {
	local name="$1" expected="$2"
	shift 2
	local out="$WORK/$name.log"
	local rc=0

	# Background + wait avoids the shell's own "Aborted (core dumped)"
	# notice for foreground jobs; wait still reports 128+signal, e.g.
	# 134 for SIGABRT.
	timeout 30 env "$@" "$CAGE" >"$out" 2>&1 &
	wait $! || rc=$?

	echo
	echo "== $name =="
	if [ "$rc" -ne 1 ]; then
		echo "FAIL: expected exit status 1, got $rc; last output:"
		fail=1
		tail -n 8 "$out" | sed 's/^/    /'
	elif grep -q "Assertion" "$out"; then
		echo "FAIL: exit status 1 but died on an assertion:"
		fail=1
		grep "Assertion" "$out" | head -3 | sed 's/^/    /'
	elif ! grep -q "$expected" "$out"; then
		echo "FAIL: expected log message '$expected' not found; last output:"
		fail=1
		tail -n 5 "$out" | sed 's/^/    /'
	else
		echo "ok: exit status 1, '$expected', no assertion"
	fi
}

mkdir -p "$WORK/xdg-ok" && chmod 700 "$WORK/xdg-ok"

# Case 1: backend creation failure (WLR_BACKENDS names an unknown
# backend). No listeners are registered at this point.
run_case "backend-creation-failure" "Unable to create the wlroots backend" \
	XDG_RUNTIME_DIR="$WORK/xdg-ok" WLR_BACKENDS=bogus WLR_RENDERER=pixman

# Case 2: socket creation failure. Every listener that main() registers
# is attached by the time cage tries to open the Wayland socket.
: >"$WORK/not-a-dir"
run_case "socket-creation-failure" "Unable to open Wayland socket" \
	XDG_RUNTIME_DIR="$WORK/not-a-dir" WLR_BACKENDS=headless WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1

if [ "$fail" -eq 0 ]; then
	echo
	echo "ALL PASS"
else
	echo
	echo "TEST FAILED"
fi
exit "$fail"
