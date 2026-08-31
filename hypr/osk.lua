-- On-screen keyboard (hypr-osk): touchscreen→pointer/keyboard compositor
-- plugin + Quickshell overlay (ekollof.osk) + bar applet (ekollof.osk-applet).
-- Toggled by swipe-up-from-the-bottom-edge (hyprgrass), SUPER+SHIFT+K, or the
-- bar applet. Installed by ~/src/omarchy-osk/install.sh; edit the bundle, not
-- this file.

-- Load the compositor plugin if built and not already loaded. Candidates:
-- install.sh's flat deploy first, then hyprpm's store (hyprpm.toml route;
-- hyprpm renames the build output to <name>.so under /var/cache/hyprpm on
-- current Hyprland).
local home     = os.getenv("HOME")
local user     = os.getenv("USER") or ""
local plug_dir = home .. "/.local/share/hyprland/plugins"
o.exec_on_start(
  "for p in " .. plug_dir .. "/libhypr-osk.so /var/cache/hyprpm/" .. user .. "/hypr-osk/hypr-osk.so; do " ..
  "test -f $p && { hyprctl plugin list | grep -q hypr-osk || hyprctl plugin load $p; break; }; done")

-- hyprgrass: preferred path is install.sh's/manual flat build, then the
-- legacy hyprpm layout (~/.local/share/...), then current hyprpm's store.
-- Load whichever exists at session start unless already loaded — hyprpm does
-- not auto-load its plugins into a fresh compositor session.
o.exec_on_start(
  "for p in " .. plug_dir .. "/hyprgrass.so " .. plug_dir .. "/hyprgrass/hyprgrass.so " ..
  "/var/cache/hyprpm/" .. user .. "/hyprgrass/hyprgrass.so; do " ..
  "test -f $p && { hyprctl plugin list | grep -q hyprgrass || hyprctl plugin load $p; break; }; done")

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
