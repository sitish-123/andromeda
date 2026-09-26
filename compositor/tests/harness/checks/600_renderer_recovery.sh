#!/usr/bin/env bash
# Renderer loss is reported from inside the renderer's mutable lost signal. The test command emits it twice in one
# dispatch: recovery must let both emissions finish, coalesce them, then replace the renderer and draw another frame.
set -euo pipefail

readonly LOG_MARK=$(($(wc -l < "$UMBRIEL_LOG") + 1))

"$UMBRIEL" renderer-recover > /dev/null

recovered=false
for _ in $(seq 100); do
  if tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -q "renderer recreated"; then
    recovered=true
    break
  fi
  sleep 0.02
done

if [[ $recovered != true ]]; then
  echo "renderer recovery did not complete"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | tail -n 20
  exit 1
fi

"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows > /dev/null

if [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "GPU context lost, recreating renderer") -ne 1 ]]; then
  echo "renderer loss did not start exactly one recovery"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | tail -n 20
  exit 1
fi

echo "renderer loss unwound, recreated the renderer, and drew another frame"
