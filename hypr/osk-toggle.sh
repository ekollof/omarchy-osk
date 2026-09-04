#!/usr/bin/bash
# Toggle the on-screen keyboard (ekollof.osk Quickshell plugin in omarchy-shell).
# Used by the hyprgrass swipe-up-from-bottom-edge gesture and SUPER+SHIFT+K.
# The plugin's open() toggles visibility. Debounced: hyprgrass can fire the
# edge gesture twice per swipe; without a lock the double toggle opens+closes.

export PATH=/usr/bin:/bin
LOCK="${XDG_RUNTIME_DIR:-/run/user/$(/usr/bin/id -u)}/osk-toggle.lock"
exec 9>>"$LOCK"
flock 9
NOW=$(date +%s%N)
LAST=0
read -r LAST <"$LOCK" || LAST=0
case "$LAST" in
  ''|*[!0-9]*) LAST=0 ;;
esac
if [ $(( (NOW - LAST) / 1000000 )) -lt 1500 ]; then
  exit 0
fi
printf '%s\n' "$NOW" >"$LOCK"

/usr/bin/omarchy-shell shell summon ekollof.osk '{}'
