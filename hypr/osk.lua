-- On-screen keyboard (hypr-osk): touchscreen→pointer/keyboard compositor
-- plugin + Quickshell overlay (ekollof.osk) + bar applet (ekollof.osk-applet).
-- Toggled by swipe-up-from-the-bottom-edge (hyprgrass), SUPER+SHIFT+K, or the
-- bar applet. Installed by ~/src/omarchy-osk/install.sh; edit the bundle, not
-- this file.

-- Load the compositor plugin if built and not already loaded.
local hypr_osk_so = os.getenv("HOME") .. "/.local/share/hyprland/plugins/libhypr-osk.so"
o.exec_on_start("test -f " .. hypr_osk_so .. " && { hyprctl plugin list | grep -q hypr-osk || hyprctl plugin load " .. hypr_osk_so .. "; }")

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
