#!/usr/bin/env bash

# Theme integration must preserve config discovery and still update existing configs.
set -euo pipefail

templates_dir=${1:?templates directory}

fail() {
  printf '%s\n' "compositor_template_config_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

configure_case() {
  local name=$1
  case_dir="$work_dir/$name"
  export HOME="$case_dir/home"
  export XDG_CONFIG_HOME="$case_dir/config"
  export XDG_DATA_HOME="$case_dir/data"
  export XDG_CONFIG_DIRS="$case_dir/system"

  compositor=$name
  case "$name" in
    andromeda)
      primary_relative=andromeda/config.toml
      theme_relative=andromeda/noctalia.toml
      integration='files = ["noctalia.toml"]'
      manual_directive="files = [\"$XDG_CONFIG_HOME/$theme_relative\"]"
      ;;
  esac

  primary="$XDG_CONFIG_HOME/$primary_relative"
  theme="$XDG_CONFIG_HOME/$theme_relative"
  system_config="$XDG_CONFIG_DIRS/$primary_relative"
  mkdir -p "$HOME/.i3" "$(dirname "$primary")" "$(dirname "$system_config")"
  printf 'system configuration must survive\n' >"$system_config"
  cp "$system_config" "$case_dir/system-before"
  printf 'generated theme\n' >"$theme"
}

run_hook() {
  bash "$templates_dir/$compositor/$1.sh" >"$case_dir/stdout" 2>"$case_dir/stderr"
}

expect_missing_config() {
  local status=0
  run_hook apply || status=$?
  [ "$status" -ne 0 ] || fail "$name: missing config reported success"
  [ ! -e "$primary" ] || fail "$name: created missing config or dangling symlink target"
  grep -qF "$primary" "$case_dir/stderr" \
    || fail "$name: diagnostic does not identify missing config"
  grep -qF "$manual_directive" "$case_dir/stderr" \
    || fail "$name: diagnostic does not show manual theme integration"
  if [ "$name" = andromeda ]; then
    grep -qF '[include.optional]' "$case_dir/stderr" \
      || fail "$name: diagnostic omits optional include section"
  fi
}

for name in andromeda; do
  configure_case "$name"

  expect_missing_config
  printf 'alternative configuration must survive\n' >"$HOME/.i3/config"
  cp "$HOME/.i3/config" "$case_dir/alternative-before"
  expect_missing_config
  [ ! -L "$primary" ] || fail "$name: created primary config symlink"
  [ -f "$theme" ] || fail "$name: removed generated theme on missing config"
  run_hook undo || fail "$name: undo failed with missing config"
  [ ! -e "$primary" ] && [ ! -L "$primary" ] \
    || fail "$name: undo created missing config"

  dangling_target="$case_dir/missing-target"
  ln -s "$dangling_target" "$primary"
  expect_missing_config
  run_hook undo || fail "$name: undo failed with dangling config symlink"
  [ ! -e "$dangling_target" ] || fail "$name: created dangling symlink target"
  [ -L "$primary" ] && [ "$(readlink "$primary")" = "$dangling_target" ] \
    || fail "$name: changed dangling config symlink"
  rm "$primary"

  printf '# user configuration\n' >"$primary"
  run_hook apply || fail "$name: apply failed with existing config: $(cat "$case_dir/stderr")"
  [ "$(grep -cF "$integration" "$primary")" = 1 ] \
    || fail "$name: existing config did not get exactly one theme integration"
  grep -qF 'user configuration' "$primary" || fail "$name: lost existing config content"
  cp "$primary" "$case_dir/applied"
  run_hook apply || fail "$name: repeated apply failed"
  cmp -s "$primary" "$case_dir/applied" || fail "$name: repeated apply changed config"

  managed_config="$case_dir/managed-config"
  mv "$primary" "$managed_config"
  ln -s "$managed_config" "$primary"
  run_hook apply || fail "$name: apply failed with configured symlink"
  [ -L "$primary" ] && [ "$(readlink "$primary")" = "$managed_config" ] \
    || fail "$name: replaced configured symlink"
  cmp -s "$managed_config" "$case_dir/applied" \
    || fail "$name: changed already-configured symlink target"
  cmp -s "$system_config" "$case_dir/system-before" || fail "$name: changed system config"
  cmp -s "$HOME/.i3/config" "$case_dir/alternative-before" || fail "$name: changed alternative config"
done
