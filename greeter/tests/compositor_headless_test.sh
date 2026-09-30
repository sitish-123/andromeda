#!/bin/sh
set -eu

compositor=$1
true_program=$2
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/noctalia-compositor-runtime.XXXXXX")
state_dir=$(mktemp -d "${TMPDIR:-/tmp}/noctalia-compositor-state.XXXXXX")
log_file="$state_dir/compositor.log"

cleanup() {
  rm -rf "$runtime_dir" "$state_dir"
}
trap cleanup EXIT INT TERM

cat >"$state_dir/greeter.toml" <<'EOF'
[output]
layout = "HEADLESS-1:0,0"
scales = "HEADLESS-1:1"
EOF

env \
  XDG_RUNTIME_DIR="$runtime_dir" \
  NOCTALIA_GREETER_STATE_DIR="$state_dir" \
  WLR_BACKENDS=headless \
  WLR_HEADLESS_OUTPUTS=2 \
  WLR_RENDERER=pixman \
  GREETER_BIN="$true_program" \
  NOCTALIA_GREETER_LOG=stderr \
  "$compositor" >"$log_file" 2>&1

grep -F "greeter output: HEADLESS-1 at (0,0)" "$log_file" >/dev/null
grep -F "output_layout: 'HEADLESS-2' not listed; placed at (1280,0)" "$log_file" >/dev/null
