#!/usr/bin/env bash
# Files FB-STORAGE at size (TASK-files-app "200 GB fixture scan returns in <= 5 s"): a home of 200 GB in 300 000
# files on its own ext4 file system. The image is sparse and the big files are fallocated, so the test needs only
# a few GB of real disk. Fails when:
#   - the Storage scan does not count about 200 GB (blocks on disk) in its categories,
#   - it takes longer than BUDGET_MS (default 5000).
# Needs mkfs.ext4 and udisksctl (loop-setup and mount as the user, no root).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-storage-large}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
BUDGET_MS=${BUDGET_MS:-5000}
SIZE_GB=${SIZE_GB:-200}
SMALL=${SMALL:-300000}
mkdir -p "$OUT"
fail() { echo "FAIL: $*"; exit 1; }
for tool in mkfs.ext4 udisksctl; do command -v "$tool" > /dev/null || fail "$tool is missing"; done

IMG=$OUT/home.img
rm -f "$IMG"
truncate -s $((SIZE_GB + 100))G "$IMG"
mkfs.ext4 -q -F -L e2ehome -E root_owner="$(id -u):$(id -g)" -N $((SMALL + 100000)) "$IMG"
LOOP=$(udisksctl loop-setup --no-user-interaction -f "$IMG" | grep -o '/dev/loop[0-9]*')
MNT=""
cleanup_loop() {
  [[ -n $MNT ]] && udisksctl unmount --no-user-interaction -b "$LOOP" > /dev/null 2>&1 || true
  udisksctl loop-delete --no-user-interaction -b "$LOOP" > /dev/null 2>&1 || true
  rm -f "$IMG"
}
MNT=$(udisksctl mount --no-user-interaction -b "$LOOP" | grep -o ' at .*' | sed 's/^ at //; s/\.$//')
[[ -d $MNT ]] || { cleanup_loop; fail "the image did not mount ($LOOP)"; }
HOME_DIR=$MNT/home
boot_headless 1

# Big files by kind (fallocated: blocks on disk, no data written) and many small files in a code tree.
python3 -I - "$HOME_DIR" "$SIZE_GB" "$SMALL" <<'PY'
import os, sys
home, size_gb, small = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
kinds = [("Videos", ".mp4", 0.45), ("Pictures", ".jpg", 0.20), ("Music", ".flac", 0.10),
         ("Documents", ".pdf", 0.10), ("Downloads", ".zip", 0.10), (".cache/app", ".bin", 0.05)]
total = size_gb * 1024**3
for folder, ext, share in kinds:
    d = os.path.join(home, folder)
    os.makedirs(d, exist_ok=True)
    count = 200
    each = int(total * share / count)
    for i in range(count):
        fd = os.open(os.path.join(d, f"f{i}{ext}"), os.O_CREAT | os.O_WRONLY, 0o644)
        os.posix_fallocate(fd, 0, each)
        os.close(fd)
per = 500
for p in range(small // per):
    d = os.path.join(home, "Code", f"p{p}", "src")
    os.makedirs(d, exist_ok=True)
    for i in range(per):
        with open(os.path.join(d, f"m{i}.c"), "wb") as f:
            f.write(b"x" * 64)
PY
echo "fixture: $(du -sh --apparent-size "$HOME_DIR" | cut -f1) in $(find "$HOME_DIR" -type f | wc -l) files; image uses $(du -sh "$IMG" | cut -f1)"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; cleanup_loop; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache")
"${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &

wait_for() { local what=$1; shift; for _ in $(seq 400); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
wait_for "the daemon is not on the bus" on_bus org.andromeda.FileOps1 /org/andromeda/FileOps1
wait_for "Files is not on the bus" on_bus "$APP" /dev/andromeda/Files
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
wait_for "the Files window did not map" mapped
# Cold cache like a first scan after login: drop this file system's cached inodes by remounting is not possible
# without root, so the timing is warm-cache (the fixture was just written).
DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
  -m org.freedesktop.Application.ActivateAction storage '[]' '{}' > /dev/null
wait_for "the storage action did not scan" grep -q "storage scan " "$OUT/files.log"
line=$(grep -o "storage scan .*" "$OUT/files.log" | tail -n 1)
echo "$line"
ms=$(sed -E 's/.* in ([0-9]+) ms.*/\1/' <<< "$line")
total=$(grep -o "storage total [0-9]*" "$OUT/files.log" | tail -n 1 | awk '{print $3}')
[[ -n $total ]] || fail "Files logged no storage total"
[[ $total -ge $((SIZE_GB * 1024 ** 3 * 95 / 100)) ]] || fail "the scan counted $total bytes, expected about $SIZE_GB GB"
[[ $ms -le $BUDGET_MS ]] || fail "the scan of $SIZE_GB GB took $ms ms, budget $BUDGET_MS ms"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/storage.png"
echo "pass files_storage_large ($SIZE_GB GB, $ms ms)"
