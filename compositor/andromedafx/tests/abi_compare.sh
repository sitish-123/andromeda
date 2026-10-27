#!/bin/sh -eu
# Compares the two abi probe builds. Field offsets must match exactly; andromedafx
# may only be larger, never smaller or differently ordered.
#
# Usage: abi_compare.sh <wlroots-probe> <andromedafx-probe>

wlroots_probe=$1
andromedafx_probe=$2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$wlroots_probe" > "$tmp/wlroots"
"$andromedafx_probe" > "$tmp/andromedafx"

status=0

# Every field wlroots declares must sit at the same offset in andromedafx.
grep '^offsetof' "$tmp/wlroots" > "$tmp/wlroots.off"
grep '^offsetof' "$tmp/andromedafx" > "$tmp/andromedafx.off"
if ! diff -u "$tmp/wlroots.off" "$tmp/andromedafx.off" > "$tmp/off.diff"; then
	echo "error: andromedafx scene struct field offsets diverge from wlroots"
	echo "       The compositor calls scene helpers andromedafx does not reimplement"
	echo "       (wlr_scene_xdg_surface_create, wlr_scene_subsurface_tree_create,"
	echo "       wlr_scene_attach_output_layout, ...) which resolve to libwlroots"
	echo "       and read these structs at wlroots offsets. A mismatch corrupts"
	echo "       memory silently."
	echo
	sed 's/^/  /' "$tmp/off.diff"
	echo
	echo "  Fix the field order in andromedafx/include/andromedafx/types/wlr_scene.h."
	echo "  Fields Andromeda adds belong after every wlroots field."
	status=1
fi

# andromedafx extends these structs, so it must never be smaller than wlroots.
grep '^sizeof' "$tmp/wlroots" > "$tmp/wlroots.size"
while read -r line; do
	name=$(printf '%s\n' "$line" | sed 's/^sizeof(struct \([a-z_]*\)).*/\1/')
	want=$(printf '%s\n' "$line" | sed 's/.*>= //')
	got=$(grep "^sizeof(struct $name)" "$tmp/andromedafx" | sed 's/.*>= //')
	if [ "$got" -lt "$want" ]; then
		echo "error: struct $name is $got bytes in andromedafx, $want in wlroots"
		echo "       andromedafx extends this struct and can never be smaller."
		status=1
	fi
done < "$tmp/wlroots.size"

exit $status
