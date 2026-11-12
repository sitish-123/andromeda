#!/usr/bin/env bash
# Usage: tools/verify.sh <feature>. Looks the feature up in docs/map.toml (exact name, else a unique where.sh match),
# then runs its build module, its test and its scenario. Prints one ok line, or the first failure and its log.
# Build and unit suites reuse tools/quiet.sh results; E2E scripts, harness checks and scenarios always run.
set -uo pipefail
[ $# -ge 1 ] || { echo "usage: tools/verify.sh <feature>" >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

found=$(python3 -I - "$root/docs/map.toml" "$*" <<'PY'
import sys
import tomllib

path, query = sys.argv[1], sys.argv[2].lower()
with open(path, "rb") as f:
    features = tomllib.load(f).get("feature", [])
hits = [f for f in features if f["name"].lower() == query]
if not hits:
    hits = [f for f in features
            if query in " ".join([f["name"], " ".join(f.get("keywords", [])), f.get("notes", "")]).lower()]
if len(hits) != 1:
    names = "; ".join(f["name"] for f in hits[:10]) or "none"
    print(f"verify: '{query}' matches {len(hits)} features ({names}); give a full name", file=sys.stderr)
    sys.exit(2)
f = hits[0]
print("\t".join([f["name"], f["build"], f.get("test", ""), f.get("scenario", "")]))
PY
) || exit 2
IFS=$'\t' read -r name build test scenario <<< "$found"
SECONDS=0

fail() {
  echo "verify $name: FAILED $1: ${2:-no error line}"
  [ -z "${3:-}" ] || echo "log: $3"
  exit 1
}

# first_error <log>: the first line that names a failure or a file:line, else the last line.
first_error() {
  sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$1" | grep -m1 -E '(^|[^A-Za-z])(FAIL|FAILED|Error|error|ERROR|Assertion)|[A-Za-z0-9_./-]+\.[a-z]+:[0-9]+' ||
    tail -n 1 "$1"
}

out=$(tools/quiet.sh build "$build" | grep -v ': still running (' | sed 's/^cached: //') ||
  fail "build $build" "$(head -n 1 <<< "$out")" "/tmp/andromeda-build/$build.log"
done_parts=("build $build")

# Shell E2E scripts default to shell/build-debug; verify built shell/build-f7, so point them at that binary.
[ "$build" = shell ] && export NOCTALIA=$root/shell/build-f7/andromeda-shell

case $test in
  '') done_parts+=("no test") ;;
  tests/e2e/*.sh)
    n=$(basename "$test" .sh) log=/tmp/andromeda-e2e/$n.log
    mkdir -p /tmp/andromeda-e2e
    bash "$test" > "$log" 2>&1
    case $? in
      0) done_parts+=("e2e $n") ;;
      77) done_parts+=("e2e $n skipped: $(tail -n 1 "$log")") ;;
      *) fail "e2e $n" "$(first_error "$log")" "$log" ;;
    esac
    ;;
  compositor/tests/harness*)
    frag=${test#compositor/tests/harness}
    frag=${frag#/}
    frag=${frag#checks/}
    frag=${frag%.sh}
    log=/tmp/andromeda-test/harness.log
    mkdir -p /tmp/andromeda-test
    NO_COLOR=1 bash compositor/tests/harness/check.sh compositor/build-debug/andromeda ${frag:+"$frag"} > "$log" 2>&1 ||
      fail "harness ${frag:-all}" "$(first_error "$log")" "$log"
    done_parts+=("harness ${frag:-all}")
    ;;
  *)
    out=$(tools/quiet.sh test "$build" | grep -v ': still running (' | sed 's/^cached: //') ||
      fail "test $build" "$(head -n 1 <<< "$out")" "/tmp/andromeda-test/$build.log"
    done_parts+=("test $build: $(grep -m1 -E '[0-9]+ passed' <<< "$out" || echo ok)")
    ;;
esac

if [ -n "$scenario" ]; then
  out=$(python3 tools/scenario.py "$scenario") || fail "scenario" "$(sed -n 2p <<< "$out" | sed 's/^ *//') ($(head -n 1 <<< "$out"))"
  done_parts+=("scenario $(basename "$scenario" .toml)")
fi

joined=$(printf '%s, ' "${done_parts[@]}")
echo "verify $name: ok (${joined%, }) ${SECONDS}s"
