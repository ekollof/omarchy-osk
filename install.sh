#!/bin/bash
# Build and deploy the omarchy-osk bundle:
#   hypr-osk           (compositor plugin, C++)  -> ~/.local/share/hyprland/plugins/
#   hyprgrass          (vendored edge gestures)  -> ~/.local/share/hyprland/plugins/
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

# 1+2. Build the compositor plugin and install it where Hyprland plugins
# live — unless hyprpm manages it (see hyprpm.toml): then its copy wins and
# a flat copy here would fight it (rebuild with: hyprpm update).
if hyprpm list 2>/dev/null | grep -q hypr-osk; then
  echo "hypr-osk is hyprpm-managed: skipping the local build (rebuild with: hyprpm update)."
else
  [[ -d "$DIR/hypr-osk/build" ]] || meson setup "$DIR/hypr-osk/build" "$DIR/hypr-osk" >/dev/null
  meson compile -C "$DIR/hypr-osk/build"
  mkdir -p "$HOME/.local/share/hyprland/plugins"
  install -m 644 "$DIR/hypr-osk/build/libhypr-osk.so" "$HOME/.local/share/hyprland/plugins/"
fi

# 3. Build the vendored hyprgrass (edge-swipe gesture) the same way: upstream
# horriblename/hyprgrass pinned to hl-0.56.1 (known-good for Hyprland 0.56.x,
# wf-touch included) — see AGENTS.md for re-vendoring. Skip when hyprpm
# manages it instead.
if hyprpm list 2>/dev/null | grep -q hyprgrass; then
  echo "hyprgrass is hyprpm-managed: skipping the vendored build (rebuild with: hyprpm update)."
else
  pacman -Q glm >/dev/null 2>&1 || omarchy pkg add glm || echo "warning: glm missing, the hyprgrass build below will fail"
  [[ -d "$DIR/vendor/hyprgrass/build" ]] || meson setup "$DIR/vendor/hyprgrass/build" "$DIR/vendor/hyprgrass" >/dev/null
  meson compile -C "$DIR/vendor/hyprgrass/build"
  install -m 644 "$DIR/vendor/hyprgrass/build/src/libhyprgrass.so" "$HOME/.local/share/hyprland/plugins/hyprgrass.so"
fi

# 4. Deploy the shell plugins
mkdir -p "$HOME/.config/omarchy/plugins"
for p in ekollof.osk ekollof.osk-applet; do
  rm -rf "$HOME/.config/omarchy/plugins/$p"
  cp -r "$DIR/shell/$p" "$HOME/.config/omarchy/plugins/"
done

# 5. Deploy the Hyprland integration
mkdir -p "$HOME/.config/hypr/scripts"
install -m 755 "$DIR/hypr/osk-toggle.sh" "$HOME/.config/hypr/scripts/osk-toggle.sh"
install -m 644 "$DIR/hypr/osk.lua" "$HOME/.config/hypr/osk.lua"
HYPRLAND="$HOME/.config/hypr/hyprland.lua"
if ! grep -q 'require("hypr.osk")' "$HYPRLAND"; then
  sed -i 's|^require("hypr.gestures")|require("hypr.gestures")\nrequire("hypr.osk")|' "$HYPRLAND"
fi

# 6. Register with the shell and reload Hyprland
omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
omarchy-shell shell putBarWidget ekollof.osk-applet '{}' >/dev/null 2>&1 || true
omarchy-shell shell setPluginEnabled ekollof.osk true >/dev/null 2>&1 || true
hyprctl reload >/dev/null 2>&1 || true

echo "omarchy-osk installed."
hyprctl plugin list | grep -q hypr-osk && echo "compositor plugin: loaded" \
  || echo "compositor plugin: NOT loaded yet (log out/in, or: hyprctl plugin load $HOME/.local/share/hyprland/plugins/libhypr-osk.so)"
hyprctl plugin list | grep -q hyprgrass && echo "hyprgrass: loaded" \
  || echo "hyprgrass: built and deployed; loads at next session (or: hyprctl plugin load $HOME/.local/share/hyprland/plugins/hyprgrass.so)"
