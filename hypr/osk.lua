-- On-screen keyboard (hypr-osk): touchscreen→pointer/keyboard compositor
-- plugin + Quickshell overlay (ekollof.osk) + bar applet (ekollof.osk-applet).
-- Toggled by swipe-up-from-the-bottom-edge (hyprgrass), SUPER+SHIFT+K, or the
-- bar applet. Installed by ~/src/omarchy-osk/install.sh; edit the bundle, not
-- this file.

-- Gamepad prototype (Steam Controller 2026): right pad = mouse, left stick
-- = scroll, triggers/pad clicks = buttons, GUIDE = OSK toggle, START =
-- Omarchy menu. Requires HYPR_OSK_GAMEPAD=1 in the compositor environment
-- (read once at plugin load). Quit Steam while testing: it reads the same
-- hidraw reports in parallel. Tune motion with HYPR_OSK_PAD_GAIN.
hl.env("HYPR_OSK_GAMEPAD", "1")
-- hl.env("HYPR_OSK_PAD_GAIN", "1.0")
hl.env("HYPR_OSK_TRACE", "1")

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

-- Physical-keyboard fallback for the same toggle.
o.bind("SUPER + SHIFT + K", "Toggle on-screen keyboard",
  os.getenv("HOME") .. "/.config/hypr/scripts/osk-toggle.sh")
