#!/usr/bin/env bash
# Toggle the on-screen keyboard (ekollof.osk Quickshell plugin in omarchy-shell).
# Used by the hyprgrass swipe-up-from-bottom-edge gesture and SUPER+SHIFT+K.
# The plugin's open() toggles visibility. Debounced: hyprgrass can fire the
# edge gesture twice per swipe; without a lock the double toggle opens+closes.

LOCK=/tmp/.osk-toggle-lock
NOW=$(date +%s%N)
LAST=0
[ -f "$LOCK" ] && LAST=$(cat "$LOCK")
if [ $(( (NOW - LAST) / 1000000 )) -lt 1500 ]; then
  exit 0
fi
echo "$NOW" > "$LOCK"

omarchy-shell shell summon ekollof.osk '{}'
