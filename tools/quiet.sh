#!/usr/bin/env bash
# Usage: tools/quiet.sh build <module> [--force]   module: compositor shell greeter portal files player viewer system link
#        tools/quiet.sh test <module> [filter] [--force]
# Runs the job with its full output in /tmp/andromeda-{build,test}/<module>.log and prints only a short summary.
# When nothing changed since the last run, prints that run's summary as "cached:" lines; --force reruns.
# Module "fake" (tests only) runs $QUIET_FAKE with its sources in $QUIET_FAKE_SRC.
# The parse-* subcommands take a saved log, for tools/tests/quiet_test.sh.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# rel <base> <path>: the path relative to the repo root; <base> is the directory the tool printed it from.
rel() {
  local p=$2
  [[ $p == /* ]] || p=$1/$p
  realpath -m --relative-to="$root" "$p"
}

strip_ansi() { sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$1"; }

# parse-build <log> <base>: the first 3 compiler or linker errors as file:line: message.
parse_build() {
  local log=$1 base=$2 f l m
  strip_ansi "$log" | awk '
    function out(f, l, m) { k = f SUBSEP l SUBSEP m; if (k in seen) return; seen[k] = 1; print f "\037" l "\037" m; if (++n >= 3) exit }
    function flush(  p) { p = pend; pend = ""; if (p != "") out("", "", p) }
    /^error(\[E[0-9]+\])?: / {
      flush()
      if ($0 ~ /could not compile|aborting due to|Some errors have/) next
      pend = $0; sub(/^error(\[E[0-9]+\])?: /, "", pend); next
    }
    pend != "" && /^ *--> / { loc = $0; sub(/^ *--> /, "", loc); split(loc, a, ":"); p = pend; pend = ""; out(a[1], a[2], p); next }
    /^[^ :]+:[0-9]+:[0-9]+: (fatal )?error: / {
      split($0, a, ":"); m = substr($0, index($0, "error: ") + 7); out(a[1], a[2], m); next
    }
    /^([^ ]*ld[^ ]*: )?[^ :]+:([0-9]+:)?\([^)]*\): / {
      s = $0; sub(/^[^ ]*ld[^ ]*: /, "", s); split(s, a, ":"); l = (a[2] ~ /^[0-9]+$/) ? a[2] : ""
      m = substr(s, index(s, "): ") + 3); out(a[1], l, m); next
    }
    /^[^ ]*(mold|lld|ld): error: / { m = substr($0, index($0, "error: ") + 7); out("", "", m); next }
    END { flush() }
  ' | while IFS=$'\037' read -r f l m; do
    if [ -z "$f" ]; then
      echo "$m"
    else
      echo "$(rel "$base" "$f")${l:+:$l}: $m"
    fi
  done
}

# fallback <log>: the ninja FAILED line or the last 3 lines, when no error line matched.
fallback() {
  local hit
  hit=$(strip_ansi "$1" | grep -m1 '^FAILED: ' | cut -c1-200)
  if [ -n "$hit" ]; then echo "$hit"; else strip_ansi "$1" | grep -v -e '^\s*$' -e 'waiting' | tail -n 3 | cut -c1-200; fi
}

# first_line <text on stdin>: the first line with file:line, else the first line that is not a log record.
first_line() {
  awk '
    /^[[:space:]]*$/ { next }
    /^[0-9][0-9]:[0-9][0-9]:[0-9][0-9]/ { next }
    !other { other = $0 }
    /[A-Za-z0-9_.\/-]+\.[A-Za-z0-9]+:[0-9]+/ { print; found = 1; exit }
    END { if (!found) print (other == "" ? "(no output)" : other) }
  ' | cut -c1-200
}

# parse-meson <testlog.json>: one line per failing test, then "N passed, M failed".
parse_meson() {
  local json=$1 name
  while IFS= read -r name; do
    printf '%s: %s\n' "${name#*:}" "$(jq -r --arg n "$name" 'select(.name == $n) | (.stdout // "") + "\n" + (.stderr // "")' "$json" \
      | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | first_line)"
  done < <(jq -r 'select(.is_fail) | .name' "$json")
  jq -s -r '"\(map(select(.result == "OK" or .result == "EXPECTEDFAIL")) | length) passed, \(map(select(.is_fail)) | length) failed"' "$json"
}

# parse-cargo <log> <base>: one line per failing test with its panic location, then "N passed, M failed".
parse_cargo() {
  local name loc
  strip_ansi "$1" | awk '
    /^test .* \.\.\. FAILED$/ { n = substr($0, 6, length($0) - 16); if (!(n in seen)) { seen[n] = 1; order[++k] = n } next }
    /^---- .* stdout ----$/ { cur = substr($0, 6, length($0) - 17); want = 0; next }
    /^test result: / { split($0, w, " "); for (i = 1; i <= NF; i++) { if (w[i] ~ /^passed/) p += w[i-1]; if (w[i] ~ /^failed/) f += w[i-1] } cur = ""; next }
    cur == "" { next }
    want { msg[cur] = loc ": " $0; want = 0; next }
    / panicked at .*:[0-9]+:[0-9]+:$/ && !(cur in msg) { loc = $0; sub(/.* panicked at /, "", loc); sub(/:[0-9]+:$/, "", loc); want = 1; next }
    / panicked at .*, [^ ]+:[0-9]+:[0-9]+$/ && !(cur in msg) {
      s = $0; sub(/.* panicked at /, "", s); l = s; sub(/.*, /, "", l); sub(/:[0-9]+$/, "", l); sub(/, [^ ]+$/, "", s); msg[cur] = l ": " s; next
    }
    !(cur in msg) && !/^[[:space:]]*$/ && !(cur in first) { first[cur] = $0 }
    END {
      for (i = 1; i <= k; i++) { n = order[i]; print n "\037" ((n in msg) ? msg[n] : ((n in first) ? first[n] : "(no output)")) }
      print "\037" (p + 0) " passed, " (f + 0) " failed"
    }
  ' | while IFS=$'\037' read -r name loc; do
    if [ -z "$name" ]; then echo "$loc"; continue; fi
    if [[ $loc =~ ^([^\ :]+\.rs):([0-9]+):\ (.*)$ ]]; then
      loc="$(rel "$2" "${BASH_REMATCH[1]}"):${BASH_REMATCH[2]}: ${BASH_REMATCH[3]}"
    fi
    printf '%s: %s\n' "$name" "${loc:0:200}"
  done
}

# run <label> <log> <cmd...>: runs the command into the log; prints a line each minute so stall watchdogs see output.
run() {
  local label=$1 log=$2 pid tick rc
  shift 2
  # Each in its own process group, so one kill stops the job's ninja/cargo/meson children too.
  setsid "$@" > "$log" 2>&1 &
  pid=$!
  setsid bash -c 'n=0; while sleep 60; do n=$((n + 60)); echo "$0: still running (${n}s)"; done' "$label" &
  tick=$!
  trap 'kill -- -"$pid" -"$tick" 2> /dev/null; exit 130' INT TERM
  wait "$pid"
  rc=$?
  kill -- -"$tick" 2> /dev/null
  trap - INT TERM
  return "$rc"
}

cargo_dir() { case $1 in system) echo system ;; link) echo link ;; *) return 1 ;; esac; }
meson_dir() {
  case $1 in
    shell) echo shell/build-f7 ;;
    compositor | portal) echo "$1/build-debug" ;;
    greeter) echo greeter/build-release ;;
    files | player | viewer) echo "apps/$1/build" ;;
    libdesk-app | indexer) echo "$1/build" ;;
    andromeda-preview) echo andromeda-preview/build-debug ;;
    *) return 1 ;;
  esac
}
modules="compositor shell greeter portal files player viewer libdesk-app andromeda-preview indexer system link"

# build-step <label> <log> <module> [targets...]: builds the module (no targets = every target) into the log.
build_step() {
  local label=$1 log=$2 m=$3 dir
  shift 3
  case $m in
    shell) run "$label" "$log" tools/f7.sh "$@" ;;
    system | link) run "$label" "$log" bash -c 'cd "$1" && cargo build --color never' _ "$m" ;;
    fake) run "$label" "$log" bash -c "${QUIET_FAKE:?}" ;;
    *) dir=$(meson_dir "$m") && run "$label" "$log" tools/build.sh ninja -C "$dir" "$@" ;;
  esac
}

# build-errors <log> <base>: the first errors of a failed build.
build_errors() {
  local errs
  errs=$(parse_build "$1" "$2")
  [ -n "$errs" ] || errs=$(fallback "$1")
  echo "$errs"
}

cmd_build() {
  local m=${1:?module} log=/tmp/andromeda-build/${1}.log base
  mkdir -p /tmp/andromeda-build
  SECONDS=0
  case $m in
    shell) base=shell/build-f7; build_step "build $m" "$log" "$m" andromeda-shell andromeda-shell.so ;;
    system | link) base=$m; build_step "build $m" "$log" "$m" ;;
    fake) base=.; build_step "build $m" "$log" "$m" ;;
    *) base=$(meson_dir "$m") || { echo "build: unknown module '$m' ($modules)"; exit 2; }; build_step "build $m" "$log" "$m" ;;
  esac
  if [ $? -eq 0 ]; then
    echo "build $m: ok (${SECONDS}s) log=$log"
    return 0
  fi
  build_errors "$log" "$base"
  echo "build $m: FAILED log=$log"
  return 1
}

cmd_test() {
  local m=${1:?module} filter=${2:-} log=/tmp/andromeda-test/${1}.log dir rc out json
  mkdir -p /tmp/andromeda-test
  SECONDS=0
  # meson test runs with --no-rebuild (a rebuild in build-f7 must go through f7.sh), so build every target first.
  if dir=$(meson_dir "$m") && ! build_step "test $m: build" "/tmp/andromeda-test/$m.build.log" "$m"; then
    build_errors "/tmp/andromeda-test/$m.build.log" "$dir"
    echo "test $m: FAILED (build failed) log=/tmp/andromeda-test/$m.build.log"
    return 1
  fi
  if dir=$(cargo_dir "$m"); then
    run "test $m" "$log" bash -c 'cd "$1" && cargo test --color never ${2:+"$2"}' _ "$dir" "$filter"
    report_cargo "$m" "$log" $? "$dir"
    return
  elif dir=$(meson_dir "$m"); then
    # A logbase per run keeps a parallel meson test in the same dir from overwriting this run's results.
    run "test $m" "$log" meson test -C "$dir" --no-rebuild --logbase "quiet-$$" ${filter:+"$filter"}
    rc=$?
    json=$dir/meson-logs/quiet-$$.json
    if [ ! -s "$json" ]; then
      fallback "$log"
      echo "test $m: FAILED (no test ran) log=$log"
      return 1
    fi
    out=$(parse_meson "$json")
    rm -f "$dir/meson-logs/quiet-$$".{json,txt,junit.xml}
  else
    echo "test: unknown module '$m' ($modules)"
    exit 2
  fi
  summarize "$m" "$log" "$rc" "$out" "$dir"
}

# report-cargo <module> <log> <rc> <base>: the summary of a cargo test run.
report_cargo() {
  local m=$1 log=$2 rc=$3 base=$4
  if [ "$rc" -ne 0 ] && ! grep -q '^test result: ' "$log"; then
    parse_build "$log" "$base" | grep . || fallback "$log"
    echo "test $m: FAILED (no test ran) log=$log"
    return 1
  fi
  summarize "$m" "$log" "$rc" "$(parse_cargo "$log" "$base")" "$base"
}

# summarize <module> <log> <rc> <parsed> <base>: failing tests, the errors of a failed run with no failing test, the count.
summarize() {
  local m=$1 log=$2 rc=$3 out=$4 base=$5
  [ "$(wc -l <<< "$out")" -le 1 ] || sed '$d' <<< "$out"
  if [ "$rc" -ne 0 ] && [[ $(tail -n 1 <<< "$out") == *" 0 failed" ]]; then
    parse_build "$log" "$base" | grep . || fallback "$log"
  fi
  echo "test $m: $(tail -n 1 <<< "$out") log=$log"
  return $((rc != 0))
}

module_dirs() {
  case $1 in
    compositor) echo compositor protocol ;;
    shell) echo shell libdesk-ui libdesk-system protocol ;;
    greeter) echo greeter libdesk-ui protocol ;;
    portal) echo portal protocol ;;
    files | player | viewer) echo "apps/$1" andromeda-preview libdesk-app libdesk-system libdesk-ui protocol ;;
    libdesk-app) echo libdesk-app libdesk-system libdesk-ui protocol motion ;;
    andromeda-preview) echo andromeda-preview libdesk-app libdesk-system libdesk-ui protocol motion ;;
    indexer) echo indexer ;;
    system | link) echo "$1" ;;
    fake) echo "${QUIET_FAKE_SRC:?}" ;;
  esac
}

out_dir() {
  case $1 in
    system | link) echo "$1/target/debug" ;;
    fake) echo "$QUIET_FAKE_SRC/build" ;;
    *) meson_dir "$1" ;;
  esac
}

# src-stamp <dirs...>: index entries, working-tree diff and untracked file contents. Git runs inside each dir: link is a submodule.
src_stamp() {
  local d
  for d; do
    echo "== $d"
    git -C "$d" ls-files -s -- .
    git -C "$d" diff HEAD -- . 2> /dev/null
    (cd "$d" && git ls-files -z -o --exclude-standard -- . | xargs -0 -r sha1sum)
  done
}

# out-stamp <module>: the build dir's options, build.ninja mtime and every executable with its mtime. Fails with no build dir.
out_stamp() {
  local d
  d=$(out_dir "$1") && [ -d "$d" ] || return 1
  sha1sum "$d/meson-info/intro-buildoptions.json" 2> /dev/null
  stat -c '%n %Y' "$d/build.ninja" 2> /dev/null
  # Cargo's build/ and incremental/ hold no outputs the tests use.
  find "$d" -maxdepth "$([ -f "$d/build.ninja" ] && echo 99 || echo 2)" -path "$d/meson-logs" -prune -o -type f -perm -u+x \
    -printf '%P %T@\n' 2> /dev/null | sort
}

# stamp <source hash> <module>: the source hash with the module's current outputs; fails with no build dir.
stamp() {
  local o
  o=$(out_stamp "$2") || return 1
  printf '%s\n%s\n' "$1" "$o" | sha256sum | cut -d' ' -f1
}

# cached <build|test> <args...>: prints the saved summary when the sources and the build dir did not change since the last
# run of the same command. --force reruns. Only results 0 and 1 are saved. The outputs are stamped after the run, as the
# run left them, so a run that rebuilt something still lets the next identical run reuse it.
cached() {
  local kind=$1 force=0 a args=() m dirs state src now out rc
  shift
  for a; do
    case $a in --force) force=1 ;; '') ;; *) args+=("$a") ;; esac
  done
  m=${args[0]:-}
  dirs=$(module_dirs "$m")
  [ -n "$dirs" ] || { "cmd_$kind" "${args[@]}"; return; }
  state=/tmp/andromeda-$kind/$m.last
  # shellcheck disable=SC2086 # dirs is a word list
  src=$({
    echo "$kind ${args[*]} ${QUIET_FAKE:-}"
    src_stamp $dirs
  } | sha256sum | cut -d' ' -f1)
  if [ "$force" -eq 0 ] && now=$(stamp "$src" "$m") && [ "$(head -n 1 "$state" 2> /dev/null)" = "$now" ]; then
    tail -n +3 "$state" | sed 's/^/cached: /'
    return "$(sed -n 2p "$state")"
  fi
  mkdir -p "${state%/*}"
  out=$(mktemp)
  "cmd_$kind" "${args[@]}" | tee "$out"
  rc=${PIPESTATUS[0]}
  rm -f "$state"
  if [ "$rc" -le 1 ] && now=$(stamp "$src" "$m"); then
    { echo "$now"; echo "$rc"; grep -v ': still running (' "$out"; } > "$state.tmp" && mv "$state.tmp" "$state"
  fi
  rm -f "$out"
  return "$rc"
}

case ${1:-} in
  build) cached build "${@:2}" ;;
  test) cached test "${@:2}" ;;
  parse-build) parse_build "$2" "$3" ;;
  parse-meson) parse_meson "$2" ;;
  parse-cargo) parse_cargo "$2" "$3" ;;
  report-cargo) report_cargo "$2" "$3" "$4" "$5" ;;
  *) echo "usage: tools/quiet.sh build|test <module> [filter] [--force]"; exit 2 ;;
esac
