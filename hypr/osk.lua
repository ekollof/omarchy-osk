-- On-screen keyboard (hypr-osk): touchscreen→pointer/keyboard compositor
-- plugin + Quickshell overlay (ekollof.osk) + bar applet (ekollof.osk-applet).
-- Toggled by swipe-up-from-the-bottom-edge (hyprgrass), SUPER+SHIFT+K, or the
-- bar applet. Installed by ~/src/omarchy-osk/install.sh; edit the bundle, not
-- this file.

-- Gamepad: auto-enabled when a Steam Controller or standard evdev pad is
-- detected. The compositor plugin starts the reader at load; injection
-- happens only while a matching controller streams. gamescope and
-- games (gamescope, steam_app, maximized/borderless or exclusive FS)
-- get the pad (device closed so they are not sharing hidraw/evdev);
-- fullscreen YouTube in a browser does not.
-- The OSK overlay takes it back. Disable from the bar
-- applet (persisted in osk.json) or set HYPR_OSK_GAMEPAD=0. Quit Steam
-- while testing the Puck: it reads the same hidraw reports in parallel.
-- hl.env("HYPR_OSK_GAMEPAD", "0")      -- kill switch at compositor load
-- hl.env("HYPR_OSK_PAD_GAIN", "0.6")   -- pointer sensitivity, 0.1–5
-- hl.env("HYPR_OSK_TRACE", "1")        -- $XDG_RUNTIME_DIR/hypr-osk-geom.log
--
-- Button/stick remap is NOT Hypr env. Edit ~/.config/omarchy/osk.json
-- (or the bar applet / `omarchy-shell ekollof.osk setGamepadMap '…'`).
-- Analog: none | leftStick | rightStick | rightPad (comma-OR for pointer).
-- Buttons: a b x y dpadUp dpadDown dpadLeft dpadRight lb rb lt rt
--          select start guide lsClick rsClick leftPadClick rightPadClick
-- Actions: none enter escape backspace space tab up down left right
--          menu toggleOsk commit close navUp navDown navLeft navRight
--          leftClick rightClick
-- desktop = OSK hidden; osk = OSK visible. Scroll stick becomes nav
-- while the keyboard is open. Unset keys keep the defaults below.
--
-- Default (copy into osk.json and edit):
--   "gamepadMap": {
--     "pointer": "rightStick,rightPad",
--     "scroll": "leftStick",
--     "desktop": {
--       "a": "enter", "b": "escape", "x": "backspace", "y": "space",
--       "dpadUp": "up", "dpadDown": "down", "dpadLeft": "left", "dpadRight": "right",
--       "select": "tab", "start": "menu", "guide": "toggleOsk",
--       "rt": "leftClick", "lt": "rightClick",
--       "rightPadClick": "leftClick", "leftPadClick": "rightClick"
--     },
--     "osk": {
--       "a": "commit", "b": "close", "x": "backspace", "y": "space",
--       "dpadUp": "navUp", "dpadDown": "navDown", "dpadLeft": "navLeft", "dpadRight": "navRight",
--       "select": "tab", "start": "menu", "guide": "toggleOsk",
--       "rt": "leftClick", "lt": "rightClick"
--     }
--   }
--
-- Nintendo-style face buttons (A/B and X/Y swapped vs Xbox):
--   "desktop": { "a": "escape", "b": "enter", "x": "space", "y": "backspace" },
--   "osk":     { "a": "close",  "b": "commit", "x": "space", "y": "backspace" }
--
-- Left stick as pointer, right stick scrolls:
--   "pointer": "leftStick", "scroll": "rightStick"
--
-- Triggers only click while the OSK is hidden (bumpers unused):
--   "desktop": { "rt": "leftClick", "lt": "rightClick", "rb": "none", "lb": "none" }

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
