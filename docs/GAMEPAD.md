# Gamepad control

Drive Omarchy from a controller: spawn the OSK, type on the grid, and use
a stick as a mouse. Two backends:

1. **Steam Controller 2026** (Puck) — hidraw VID `28de`, PID `1302`/`1304`,
   report `0x42` (USB or Bluetooth if that report still streams).
2. **Standard Linux gamepads** — evdev nodes with `BTN_GAMEPAD` / `BTN_SOUTH`
   (Bluetooth DualSense, Xbox, 8BitDo, Switch Pro, …). No grab on that
   node, so games still see the pad.

Valve hidraw wins when both are present. Mapping is `gamepadMap` in
`~/.config/omarchy/osk.json` (allowlisted tokens; the bar applet edits
the common bindings).

There is no Hyprland gamepad plugin (upstream closed it as "use userspace
tools", and the Puck exposes no kernel gamepad device at all — only
lizard-mode mouse/keyboard plus raw hidraw). So `hypr-osk` reads the pad's
hidraw report `0x42` directly and reuses its in-compositor injection
(synthetic keyboard, seat pointer buttons, scroll, cursor motion).

Wire layout: `s3govesus/steam-controller-x`
`crates/sc-protocol/src/report.rs` — the only public decode of this
hardware. Verified live against our unit (54-byte reports @~270 Hz on the
paired puck channel; idle channels stay silent).

## Layout

Mouse (always live):

| Control          | Action                                              |
|------------------|-----------------------------------------------------|
| Right stick      | Move pointer (deflection velocity, rest-calibrated) |
| Right pad        | Move pointer (absolute deltas, accel curve)         |
| Right pad click  | Left click                                          |
| Right trigger    | Left click (hair-trigger ~1/3 pull)                 |
| Left pad click   | Right click                                         |
| Left trigger     | Right click (hair-trigger ~1/3 pull)                |
| Left stick       | Scroll (both axes, respects scroll gain)            |

Buttons, OSK hidden (desktop duty):

| Control | Action                    |
|---------|---------------------------|
| A / B   | Enter / Escape            |
| X / Y   | Backspace / Space         |
| D-pad   | Arrow keys                |
| Back    | Tab                       |
| START   | Omarchy menu (SUPER+SPACE)|
| GUIDE   | OSK toggle (SUPER+SHIFT+K)|

Buttons, OSK visible (Steam-like grid typing):

| Control          | Action                                  |
|------------------|-----------------------------------------|
| D-pad / L-stick  | Move highlight (hold to repeat)         |
| A                | Commit highlighted key (hold repeats)   |
| X / Y            | Backspace / Space                       |
| B                | Close keyboard                          |
| START / GUIDE    | Unchanged (menu / toggle)               |

Chords go through the synthetic keyboard so real compositor keybinds fire;
no new IPC was needed for them. Grid nav arrives as unsolicited
`nav <up|down|left|right|commit|back|space|close> <1|0>` socket lines; the
QML owns highlight, repeat and commit via its normal `activate()` path.
Bumpers, paddles and the "dots" button are unmapped (free for later).

## Bar icon + toggle

The reader starts with the compositor plugin and injects only while a
matching controller streams (plug in / wake the pad and it works). The
OSK bar applet shows a gamepad icon next to the keyboard icon only while
a controller is attached. Click the icon to disable the reader; the
panel's Gamepad section has the same toggle plus connection status.
State persists in `osk.json` (`gamepad`) and reconciles on every handshake.

Socket: `GAMEPAD on|off|toggle` (pinned-shell only, replies `ok`) and
`GAMEPAD query` (replies `pad <enabled01> <active01>` inline). Changes
arrive as unsolicited `pad <enabled> <active>` pushes. Disabling closes
the hidraw fd, releases grabs/held inputs and sleeps without polling.

## Enable / disable

On by default: a matching Steam Controller **or** a standard evdev
gamepad works as soon as it streams. No env var required.

The reader **yields** (stops injecting, keeps the device open) when the
focused window is `gamescope` (class/title) or exclusive-fullscreen
**except** browsers and media players (Firefox, Chromium, Brave, mpv,
VLC, …). Fullscreen YouTube keeps the pad as a mouse. A Steam/gamescope
game gets the pad. Showing the OSK overlay resumes injection. Maximized
(not fullscreen) windows never yield.

## Mapping (`osk.json` `gamepadMap`)

Allowlisted only — unknown tokens are dropped. Analog sources: `none`,
`leftStick`, `rightStick`, `rightPad` (comma-OR for pointer). Buttons:
`a` `b` `x` `y` `dpadUp` `dpadDown` `dpadLeft` `dpadRight` `lb` `rb` `lt`
`rt` `select` `start` `guide` `lsClick` `rsClick` `leftPadClick`
`rightPadClick`. Actions: `none` `enter` `escape` `backspace` `space`
`tab` `up` `down` `left` `right` `menu` `toggleOsk` `commit` `close`
`navUp` `navDown` `navLeft` `navRight` `leftClick` `rightClick`.

`desktop` is keyboard-hidden; `osk` is keyboard-visible. The scroll stick
becomes D-pad nav while the OSK is open. `rightPad` / pad-clicks only
produce data on the Steam hidraw path.

```json
"gamepadMap": {
  "pointer": "rightStick,rightPad",
  "scroll": "leftStick",
  "desktop": { "a": "enter", "guide": "toggleOsk", "rt": "leftClick" },
  "osk": { "a": "commit", "b": "close" }
}
```

`omarchy-shell ekollof.osk setGamepadMap '{"pointer":"leftStick",...}'` or
the bar applet dropdowns. Unset keys keep the compiled defaults.

```lua
-- hypr/osk.lua — optional; the plugin auto-starts the reader
-- hl.env("HYPR_OSK_GAMEPAD", "0")     -- kill switch at compositor load
-- hl.env("HYPR_OSK_PAD_GAIN", "0.6")  -- pointer sensitivity, 0.1–5
-- hl.env("HYPR_OSK_TRACE", "1")       -- $XDG_RUNTIME_DIR/hypr-osk-geom.log
```

Bar applet toggle (or `omarchy-shell ekollof.osk setGamepad off`) parks the
reader. `STATS` shows `pad=1 padbtn=<hex>` when it holds the device.

## Constraints (do not re-learn)

- **Quit Steam while testing.** hidraw has no exclusive open: a running
  Steam client consumes the same reports and acts on its own (mis)parse in
  parallel. Its desktop cursor is separately broken on Hyprland (Valve
  #13185; `lib32-extest` workaround). The plugin does not scan `/proc` for
  Steam; this is a usage constraint, not a runtime check.
- **Phantom lizard-mode nodes are EVIOCGRABbed** while gamepad mode holds
  the device (8 nodes: 4 mouse + 4 keyboard), so kernel events don't double
  the synthetic ones. Released on device loss / plugin unload.
- **Byte 4 bit 0x80 is the trigger bottom-out click, not a pad button.**
  It fires at full trigger pull and used to alias to a right-click; it is
  masked out of the button word and the real left-pad click is read from
  the STATUS byte (offset 5) bit 0x04 instead.
- **Trigger L/R labels verified by capture**, not by the public decode
  alone: left trigger = u16le @6, right = u16le @8, 0..32767.
- **Sticks don't rest at 0** (~±500 observed): rest center is calibrated
  from the first 30 reports at device open, deadzone 1500 after that.
- **Pad motion is per-report deltas** with a 150-unit noise deadband,
  flick acceleration and an 8000-unit teleport guard — not a flat gain
  (flat gain random-walks the cursor at 270 Hz).
- **Pointer motion goes through `onMouseMoved`**, not bare `warpTo`:
  warp alone never clears `hide_on_key_press`, so the cursor stayed
  invisible after any pad button press until a real device moved.
- **Nav needs the visibility gate**: the OSK re-PANELs 800 ms after show
  because the layer maps asynchronously — a pre-map PANEL wedges the
  plugin at `panel_valid=0` (no TEXT/KEY/nav) with no later retry.
- **MON follows the pointer** when no touch device is bound (touch frame
  wins while bound so taps stay in-frame); the shell docks on that monitor.
- Reader thread never calls compositor APIs or writes the client socket
  (ring + coalesced PADWAKE/PADSTATE, same discipline as the socket
  thread); teardown joins it before unmap; no event-loop timers of its own.
- HID match is `HID_ID=` vendor/product (`28de` + `1302`/`1304`), not a
  substring anywhere in uevent. Phantom grabs name-probe `O_RDONLY` first.

## Test checklist

1. Steam quit. `./install.sh`, `hyprctl reload`, plugin reload.
   Geom log (`$XDG_RUNTIME_DIR/hypr-osk-geom.log` when `HYPR_OSK_TRACE=1`):
   `gamepad: streaming hidraw open, phantoms grabbed=8`.
2. Right stick / right pad move a *visible* cursor (press A first: the
   cursor hides on keypress and stick motion must re-show it).
3. RT = left click, LT = right click; full RT pull = exactly one left
   click (geom: `pad ptr btn=272 down/up` only, no 273).
4. GUIDE summons the OSK on the pointer's monitor; D-pad moves the
   highlight; A commits (hold repeats); X/Y backspace/space; B closes.
   Repeat open/close several times — every open must end nav-live
   (`panel rect valid=1` in the geom log).
5. `omarchy-shell ekollof.osk getState` still `gridLoaded: true`
   (QML client survived); `omarchy restart shell` serves the newest
   `rev<N>` marker when QML changes don't land.
