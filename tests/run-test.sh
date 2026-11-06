#!/bin/sh
# Runs a test client as cage's application on the headless backend; cage
# propagates the client's exit code when the client exits.
#
# Usage: run-test.sh <cage> <client>
set -eu

if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -w "${XDG_RUNTIME_DIR:-/nonexistent}" ]; then
	XDG_RUNTIME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/cage-test.XXXXXX")"
	export XDG_RUNTIME_DIR
	trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
fi

export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
unset WAYLAND_DISPLAY DISPLAY

# No exec: the EXIT trap above must fire to clean up the temporary runtime
# dir. set -e propagates a failing exit code.
"$1" -- "$2"
