#!/usr/bin/env bash
# Task View overview (thumb row below the main area): drag_to_thumb.sh with a floating window, which a thumb or + drop
# moves without sending it off screen.
DRAG_TO_THUMB_STYLE=taskview DRAG_TO_THUMB_FLOATING=true source "$(dirname "${BASH_SOURCE[0]}")/drag_to_thumb.sh"
