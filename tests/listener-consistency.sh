#!/bin/bash
# Static consistency check for the signal listeners registered in cage.c.
#
# All startup error paths jump to the shared "end:" cleanup, which runs
# before wl_display_destroy(). wlroots objects destroyed inside
# wl_display_destroy() assert that their signals' listener lists are
# empty (e.g. types/wlr_output_layout.c, xwayland/xwayland.c), so every
# listener that main() registers via wl_signal_add() must follow this
# protocol:
#
#   1. wl_list_init(&server.<name>.link) before main() reaches the first
#      "goto end" - a zero-initialized node cannot be wl_list_remove()d.
#
#   2. a wl_list_remove(&server.<name>.link) inside the "end:" block,
#      before the wl_display_destroy() call.
#
# EXEMPT_BEFORE_END lists listeners that are additionally removed before
# "end:" on the normal path: the XWayland listener must be detached
# before wl_display_destroy_clients() disconnects the XWayland client,
# so the normal path removes it early and the "end:" block only removes
# it when an error path skipped the early removal.
#
# The check ends with a self-test: a mutated copy of cage.c with one
# wl_list_init() line deleted must be rejected. If the parsing below
# ever rots into vacuously passing, the self-test catches it.
#
# Usage: tests/listener-consistency.sh [path-to-cage.c]
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/cage.c}"

[ -f "$SRC" ] || { echo "cage.c not found: $SRC" >&2; exit 1; }

EXEMPT_BEFORE_END="new_xwayland_surface"

fail=0
violation() {
	echo "FAIL: $1"
	fail=1
}

# Flatten the source so calls split across lines still match.
flat() { tr '\n' ' ' <"$SRC" | tr -s '[:space:]' ' '; }
added() { flat | grep -oE 'wl_signal_add\([^)]*&server\.[a-z0-9_]+' | sed 's/.*&server\.//' | sort -u; }

first_goto=$(grep -nF 'goto end;' "$SRC" | head -1 | cut -d: -f1)
end_line=$(grep -n '^end:$' "$SRC" | head -1 | cut -d: -f1)
destroy_line=$(grep -nF 'wl_display_destroy(server.wl_display)' "$SRC" | head -1 | cut -d: -f1)

[ -n "$first_goto" ] || { echo "FAIL: no 'goto end;' found in $SRC" >&2; exit 1; }
[ -n "$end_line" ] || { echo "FAIL: no 'end:' label found in $SRC" >&2; exit 1; }
[ -n "$destroy_line" ] || { echo "FAIL: wl_display_destroy(server.wl_display) not found in $SRC" >&2; exit 1; }

# R1 + R2: every registered listener must be initialized before the
# first error path and removed inside the shared cleanup.
for name in $(added); do
	init_line=$(grep -nF "wl_list_init(&server.$name.link)" "$SRC" | head -1 | cut -d: -f1)
	if [ -z "$init_line" ]; then
		violation "listener '$name' is wl_signal_add'ed but never wl_list_init'ed (required before the first 'goto end;' at line $first_goto)"
	elif [ "$init_line" -gt "$first_goto" ]; then
		violation "wl_list_init(&server.$name.link) at line $init_line is after the first 'goto end;' (line $first_goto); move it before any error path"
	fi

	remove_count=$(awk -v n="$end_line" -v d="$destroy_line" 'NR > n && NR < d' "$SRC" |
		grep -cF "wl_list_remove(&server.$name.link)")
	if [ "${remove_count:-0}" -eq 0 ]; then
		violation "listener '$name' has no wl_list_remove in the 'end:' cleanup (between lines $end_line and $destroy_line)"
	fi
done

# R3: no dead inits.
for name in $(grep -oE 'wl_list_init\(&server\.[a-z0-9_]+\.link\)' "$SRC" |
	sed -E 's/.*&server\.([a-z0-9_]+)\.link.*/\1/' | sort -u); do
	added | grep -qxF "$name" ||
		violation "wl_list_init(&server.$name.link) but '$name' is never wl_signal_add'ed (dead init?)"
done

# R4: removes outside the shared cleanup are only allowed for the
# exempt list; every remove must belong to a registered listener.
while IFS=: read -r line rest; do
	[ -n "${line:-}" ] || continue
	name=$(printf '%s' "$rest" | sed -nE 's/.*wl_list_remove\(&server\.([a-z0-9_]+)\.link\).*/\1/p')
	[ -n "$name" ] || continue
	if [ "$line" -lt "$end_line" ]; then
		case " $EXEMPT_BEFORE_END " in
		*" $name "*) ;;
		*)
			violation "wl_list_remove(&server.$name.link) at line $line is before 'end:' (line $end_line); error paths would skip it"
			;;
		esac
	else
		added | grep -qxF "$name" ||
			violation "wl_list_remove(&server.$name.link) at line $line does not match any wl_signal_add'ed listener"
	fi
done < <(grep -nE 'wl_list_remove\(&server\.[a-z0-9_]+\.link\)' "$SRC")

# Self-test: a copy with one wl_list_init() removed must be rejected.
# Skipped while the source has no inits at all (pre-fix tree).
if [ "${LISTENER_CONSISTENCY_NESTED:-0}" != "1" ]; then
	mutated="$(mktemp).c"
	awk '!skip && /wl_list_init\(&server\./ { skip = 1; next } { print }' "$SRC" >"$mutated"
	if cmp -s "$mutated" "$SRC"; then
		echo "note: no wl_list_init lines yet; self-test skipped"
	else
		LISTENER_CONSISTENCY_NESTED=1 "$0" "$mutated" >/dev/null 2>&1 &&
			violation "self-test: mutated cage.c (one wl_list_init removed) was accepted"
		rm -f "$mutated"
	fi
fi

if [ "$fail" -eq 0 ]; then
	echo "LISTENER CONSISTENCY OK ($(added | wc -l) listeners)"
else
	echo
	echo "LISTENER CONSISTENCY FAILED"
fi
exit "$fail"
