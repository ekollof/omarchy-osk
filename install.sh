#!/bin/bash
# Build and deploy the omarchy-osk bundle:
#   hypr-osk           (compositor plugin, C++)  -> ~/.local/share/hyprland/plugins/
#   ekollof.osk        (Quickshell overlay)      -> ~/.config/omarchy/plugins/
#   ekollof.osk-applet (bar widget)              -> ~/.config/omarchy/plugins/
#   hypr/osk.lua       (plugin load, gesture, keybind)
#   hypr/osk-toggle.sh                           -> ~/.config/hypr/scripts/
#
# Idempotent: safe to re-run after every edit. The shell hot-reloads plugin
# code, but a stale component cache can serve old QML — run `omarchy restart
# shell` if changes don't land.

set -euo pipefail
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# 1. Build the compositor plugin
[[ -d "$DIR/hypr-osk/build" ]] || meson setup "$DIR/hypr-osk/build" "$DIR/hypr-osk" >/dev/null
meson compile -C "$DIR/hypr-osk/build"

# 2. Install it where Hyprland plugins live (autostart loads from here)
mkdir -p "$HOME/.local/share/hyprland/plugins"
install -m 644 "$DIR/hypr-osk/build/libhypr-osk.so" "$HOME/.local/share/hyprland/plugins/"

# 3. Deploy the shell plugins
for p in ekollof.osk ekollof.osk-applet; do
  rm -rf "$HOME/.config/omarchy/plugins/$p"
  cp -r "$DIR/shell/$p" "$HOME/.config/omarchy/plugins/"
done

# 4. Deploy the Hyprland integration
mkdir -p "$HOME/.config/hypr/scripts"
install -m 755 "$DIR/hypr/osk-toggle.sh" "$HOME/.config/hypr/scripts/osk-toggle.sh"
install -m 644 "$DIR/hypr/osk.lua" "$HOME/.config/hypr/osk.lua"
HYPRLAND="$HOME/.config/hypr/hyprland.lua"
if ! grep -q 'require("hypr.osk")' "$HYPRLAND"; then
  sed -i 's|^require("hypr.gestures")|require("hypr.gestures")\nrequire("hypr.osk")|' "$HYPRLAND"
fi

# 5. Register with the shell and reload Hyprland
omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
omarchy-shell shell putBarWidget ekollof.osk-applet '{}' >/dev/null 2>&1 || true
omarchy-shell shell setPluginEnabled ekollof.osk true >/dev/null 2>&1 || true
hyprctl reload >/dev/null 2>&1 || true

echo "omarchy-osk installed."
hyprctl plugin list | grep -q hypr-osk && echo "compositor plugin: loaded" \
  || echo "compositor plugin: NOT loaded yet (log out/in, or: hyprctl plugin load $HOME/.local/share/hyprland/plugins/libhypr-osk.so)"
if ! hyprctl plugin list | grep -q hyprgrass; then
  echo "hyprgrass not loaded: the edge-swipe gesture is unavailable."
  echo "  Install it from the AUR:  omarchy pkg aur add hyprgrass-meta"
  echo "  The keyboard still toggles via SUPER+SHIFT+K and the bar applet."
fi
