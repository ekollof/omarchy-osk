-- On-screen keyboard (hypr-osk): touchscreen→pointer/keyboard compositor
-- plugin + Quickshell overlay (ekollof.osk) + bar applet (ekollof.osk-applet).
-- Toggled by swipe-up-from-the-bottom-edge (hyprgrass), SUPER+SHIFT+K, or the
-- bar applet. Installed by ~/src/omarchy-osk/install.sh; edit the bundle, not
-- this file.

-- Gamepad (Steam Controller 2026): auto-enabled when the pad is detected.
-- The compositor plugin starts the hidraw reader at load; injection happens
-- only while a matching controller streams. Disable from the bar applet
-- (persisted in osk.json) or set HYPR_OSK_GAMEPAD=0. Quit Steam while
-- testing: it reads the same hidraw reports in parallel.
-- hl.env("HYPR_OSK_GAMEPAD", "0")      -- kill switch at compositor load
-- hl.env("HYPR_OSK_PAD_GAIN", "0.6")   -- pointer sensitivity, 0.1–5
-- hl.env("HYPR_OSK_TRACE", "1")        -- $XDG_RUNTIME_DIR/hypr-osk-geom.log

-- Load the compositor plugin if built and not already loaded. Candidates:
-- install.sh's flat deploy first, then hyprpm's store (hyprpm.toml route;
-- hyprpm renames the build output to <name>.so under /var/cache/hyprpm on
-- current Hyprland).
local home     = os.getenv("HOME")
local user     = os.getenv("USER") or ""
local plug_dir = home .. "/.local/share/hyprland/plugins"

-- hyprgrass first, hypr-osk last: both listen to the same cancellable touch
-- bus, and the last writer of `cancelled` wins. hypr-osk must be able to
-- consume 1–2 finger contacts for the virtual pointer.
-- Load whichever exists at session start unless already loaded — hyprpm does
-- not auto-load its plugins into a fresh compositor session.
o.exec_on_start(
  "for p in " .. plug_dir .. "/hyprgrass.so " .. plug_dir .. "/hyprgrass/hyprgrass.so " ..
  "/var/cache/hyprpm/" .. user .. "/hyprgrass/hyprgrass.so; do " ..
  "test -f $p && { hyprctl plugin list | grep -q hyprgrass || hyprctl plugin load $p; break; }; done")

o.exec_on_start(
  "for p in " .. plug_dir .. "/libhypr-osk.so /var/cache/hyprpm/" .. user .. "/hypr-osk/hypr-osk.so; do " ..
  "test -f $p && { hyprctl plugin list | grep -q hypr-osk || hyprctl plugin load $p; break; }; done")

-- Swipe up from the bottom edge: toggle the on-screen keyboard (hyprgrass).
if hl.plugin and hl.plugin.hyprgrass then
  hl.plugin.hyprgrass.bind {
    pattern = { kind = "edge", origin = "d", direction = "u" },
    action = hl.dsp.exec_cmd(os.getenv("HOME") .. "/.config/hypr/scripts/osk-toggle.sh"),
  }
end

-- Physical-keyboard toggle: direct summon, NO debounce. Compositor keybinds
-- fire once per press, so every press toggles. (The swipe path below keeps
-- the debounced script: hyprgrass fires the edge gesture twice per swipe,
-- and sharing the 1500 ms lock made fast double-taps of this combo eat
-- their own second press.)
o.bind("SUPER + SHIFT + K", "Toggle on-screen keyboard",
  "omarchy-shell shell summon ekollof.osk '{}'")
