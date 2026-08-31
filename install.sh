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

# 3. Deploy the shell plugins
mkdir -p "$HOME/.config/omarchy/plugins"
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

# hyprgrass (edge-swipe gesture) is not in the Arch repos. Omarchy-specific,
# so drive hyprpm (Hyprland's plugin manager) directly as the user: it builds
# hyprgrass against the RUNNING compositor and enables it. Needs gcc, meson,
# ninja, glm, git, network — and a terminal for hyprpm's sudo prompts.
if ! hyprctl plugin list | grep -q hyprgrass; then
  missing=()
  for dep in git gcc meson ninja glm; do
    pacman -Q "$dep" >/dev/null 2>&1 || missing+=("$dep")
  done
  if ((${#missing[@]})) && ! omarchy pkg add "${missing[@]}"; then
    echo "warning: could not install hyprgrass build deps: ${missing[*]}"
  fi
  if hyprpm list 2>/dev/null | grep -q hyprgrass; then
    hyprpm update hyprgrass || true
  else
    hyprpm add https://github.com/horriblename/hyprgrass || true
  fi
  if hyprpm enable hyprgrass && hyprctl plugin list | grep -q hyprgrass; then
    echo "hyprgrass installed via hyprpm: edge-swipe gesture active."
    echo "  After Hyprland updates, rebuild it with:  hyprpm update"
  else
    echo "hyprgrass install via hyprpm failed: the edge-swipe gesture is missing."
    echo "  Run in a terminal:  hyprpm add https://github.com/horriblename/hyprgrass"
    echo "                      hyprpm enable hyprgrass"
    echo "  The keyboard still toggles via SUPER+SHIFT+K and the bar applet."
  fi
fi
