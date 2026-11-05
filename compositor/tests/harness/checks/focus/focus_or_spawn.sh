#!/usr/bin/env bash
# focus-or-spawn: with no window of the app it runs the command; with one, it focuses that window and starts nothing;
# from another app's window, it brings focus back to the app's most recently focused window.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

command -v foot > /dev/null || { echo "foot is not available"; exit 1; }

windows() { "$ANDROMEDA" windows --json; }
count() { windows | jq "[.[] | select(.app_id == \"$1\")] | length"; }
focused_app() { windows | jq -r '.[] | select(.focused) | .app_id'; }
wait_for() {
  local query=$1 message=$2
  for _ in $(seq 100); do
    windows | jq -e "$query" > /dev/null && return 0
    sleep 0.05
  done
  echo "$message: $(windows)"
  exit 1
}

fos() { "$ANDROMEDA" msg "focus-or-spawn:$1 foot --config=/dev/null --app-id=$1 sh -c 'sleep 60'" > /dev/null; }

fos fos-a
wait_for '[.[] | select(.app_id == "fos-a")] | length == 1' "the first press did not open fos-a"

fos fos-a
# A second process would map within this time: foot maps in well under a second here.
for _ in $(seq 10); do
  [[ $(count fos-a) -eq 1 ]] || { echo "the second press opened another fos-a window"; exit 1; }
  sleep 0.1
done

"$ANDROMEDA" msg "spawn:foot --config=/dev/null --app-id=fos-b sh -c 'sleep 60'" > /dev/null
wait_for '.[] | select(.focused) | .app_id == "fos-b"' "fos-b did not take focus"

fos FOS-A
wait_for '.[] | select(.focused) | .app_id == "fos-a"' "focus-or-spawn did not focus fos-a (case-insensitive)"
[[ $(count fos-a) -eq 1 ]] || { echo "focusing fos-a opened another window"; exit 1; }
[[ $(focused_app) == fos-a ]]
echo "focus-or-spawn spawns once, then focuses the app's window"
