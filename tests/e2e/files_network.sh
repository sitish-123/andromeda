#!/usr/bin/env bash
# Files FB-NET over sftp: a private sshd on 127.0.0.1:2222 and gvfs on the test bus. Fails when:
#   - `andromeda-files --reconnect` does not mount a saved server marked "reconnect" (gio mount -l lists it),
#   - it mounts a saved server that is not marked,
#   - Files does not list the share's file through the gvfs mount, or Eject does not unmount it.
# ssh runs through a wrapper that adds the test key, so the user's own ~/.ssh is never read.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-network}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
HOME_DIR=$RUNTIME/home
SSHD_DIR=$RUNTIME/sshd
SHARE=$RUNTIME/share
USER_NAME=$(id -un)
mkdir -p "$OUT" "$HOME_DIR/.config/andromeda/files" "$SSHD_DIR" "$SHARE" "$RUNTIME/bin"
fail() { echo "FAIL: $*"; exit 1; }
command -v sshd > /dev/null || { echo "SKIP: sshd is not installed"; exit 0; }
[[ -x /usr/lib/gvfsd-sftp ]] || { echo "SKIP: gvfs sftp backend is not installed"; exit 0; }
echo "remote" > "$SHARE/remote.txt"

ssh-keygen -q -t ed25519 -N "" -f "$SSHD_DIR/host" > /dev/null
ssh-keygen -q -t ed25519 -N "" -f "$SSHD_DIR/client" > /dev/null
cp "$SSHD_DIR/client.pub" "$SSHD_DIR/authorized_keys"
cat > "$SSHD_DIR/sshd_config" <<CFG
Port 2222
ListenAddress 127.0.0.1
HostKey $SSHD_DIR/host
AuthorizedKeysFile $SSHD_DIR/authorized_keys
PidFile $SSHD_DIR/sshd.pid
StrictModes no
UsePAM no
PasswordAuthentication no
Subsystem sftp internal-sftp
CFG
# gvfsd-sftp runs `ssh` from PATH: this one uses the test key and trusts the test host.
cat > "$RUNTIME/bin/ssh" <<SH
#!/bin/sh
exec /usr/bin/ssh -F /dev/null -i "$SSHD_DIR/client" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null -o BatchMode=yes "\$@"
SH
chmod +x "$RUNTIME/bin/ssh"
/usr/bin/sshd -D -e -f "$SSHD_DIR/sshd_config" > "$OUT/sshd.log" 2>&1 &

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_all=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config"
  XDG_RUNTIME_DIR="$RUNTIME" PATH="$RUNTIME/bin:$PATH" GIO_USE_VFS=gvfs)
"${env_all[@]}" /usr/lib/gvfsd --no-fuse > "$OUT/gvfsd.log" 2>&1 &
for _ in $(seq 100); do
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d org.gtk.vfs.Daemon -o /org/gtk/vfs/Daemon \
    > /dev/null 2>&1 && break
  sleep 0.05
done
for _ in $(seq 100); do (exec 3<> /dev/tcp/127.0.0.1/2222) 2> /dev/null && break; sleep 0.05; done

URI="sftp://$USER_NAME@127.0.0.1:2222$SHARE"
printf '[{"name":"Test share","uri":"%s","reconnect":true},{"name":"Other","uri":"sftp://%s@127.0.0.1:2223/","reconnect":false}]\n' \
  "$URI" "$USER_NAME" > "$HOME_DIR/.config/andromeda/files/servers.json"

"${env_all[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" --reconnect > "$OUT/reconnect.log" 2>&1 \
  || { cat "$OUT/reconnect.log"; fail "--reconnect did not mount the saved server"; }
mounts=$("${env_all[@]}" gio mount -l 2>&1)
grep -q "127.0.0.1" <<< "$mounts" || { echo "$mounts"; fail "gio mount -l does not list the reconnected server"; }
grep -q "2223" "$OUT/reconnect.log" && fail "--reconnect tried a server that is not marked"
"${env_all[@]}" gio list "$URI/" | grep -q remote.txt || fail "the reconnected share does not list remote.txt"
"${env_all[@]}" gio mount -u "$URI" || fail "the share did not unmount"
echo "pass files_network"
