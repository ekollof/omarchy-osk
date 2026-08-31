# omarchy-osk

On-screen keyboard + virtual touchscreen pointer device for
[Omarchy](https://omarchy.org) (Hyprland + Quickshell). Turns a touchscreen
laptop/tablet into a fully usable machine without a physical keyboard: the
compositor plugin consumes all touchscreen input and re-emits it as pointer
input, while the on-screen keyboard injects text and key events into whatever
window has focus — exactly like wvkbd, but native to the Omarchy shell.

## Components

| Component | What it is |
|---|---|
| `hypr-osk/` | Hyprland compositor plugin (C++23, meson): touch→pointer emulation, a synthetic keyboard device, and a unix-socket IPC server |
| `shell/ekollof.osk/` | Quickshell overlay plugin: the visible keyboard (wvkbd-style terminal layout) |
| `shell/ekollof.osk-applet/` | Bar widget: enable/disable, show/hide, layout picker, key-repeat settings |
| `hypr/osk.lua` | Hyprland integration: plugin autostart, gesture bind, SUPER+SHIFT+K |
| `hypr/osk-toggle.sh` | Debounced toggle script |
| `vendor/hyprgrass/` | Vendored hyprgrass (upstream `hl-0.56.1`, incl. wf-touch) for the edge-swipe gesture |
| `hyprpm.toml` | Optional: build the compositor plugin with `hyprpm` instead of `install.sh` |

## How it works

- **Touch → pointer**: the plugin consumes all touchscreen input and
  re-emits it as pointer input — one finger = cursor under the finger +
  left click (drag = move), two fingers = scroll (touchpad-style pixels,
  1:1 with the hand), quick two-finger tap = right click, 3+ fingers are
  left to hyprgrass for workspace gestures.
- **Key tap → typed text**: taps inside the keyboard panel become synthetic
  clicks on the keyboard layer (keyboard focus never leaves the target
  window). Keys travel over a unix socket to the compositor plugin, which
  resolves each character against the *active* xkb keymap of its synthetic
  keyboard device and injects real evdev keycodes — compositor keybinds and
  dead-key composition just work.
- **Layouts**: any layout installed under `/usr/share/X11/xkb`
  (`LAYOUT dk`, `us(intl)`, …). The plugin dumps the letter grid of the
  active keymap to the shell, so labels always match what typing produces.
  The default comes from the session locale (`da_DK` → `dk`, …).

## Install

```bash
./install.sh
```

Builds both compositor plugins (hypr-osk + vendored hyprgrass), deploys the
shell plugins, the Hyprland glue and registers everything. Idempotent —
re-run after every edit. Requires Omarchy (Hyprland 0.56.x, quickshell),
meson/ninja/gcc, glm; `install.sh` installs missing packages via
`omarchy pkg add`.

`hyprpm` is an alternative route for the compositor plugin
(`hyprpm update && hyprpm add <repo-url> && hyprpm enable hypr-osk`); the
local install is the primary path and `install.sh` defers to hyprpm when it
manages a plugin.

## Usage

Toggle the keyboard:

- swipe up from the bottom screen edge (hyprgrass),
- SUPER+SHIFT+K,
- bar applet: keyboard icon (left-click = settings, right-click = toggle).

Touch gestures: tap = click, drag = move, two-finger drag = scroll,
two-finger tap = right click, three+ fingers = hyprgrass workspace
gestures. On-screen keys: hold backspace/arrows (or any key) for
auto-repeat; sticky Ctrl/Alt/Super and one-shot shift behave phone-style.

Settings live in `~/.config/omarchy/osk.json` (layout, repeat
enabled/delay/interval) and are editable from the bar applet or the IPC:

```bash
omarchy-shell ekollof.osk getState
omarchy-shell ekollof.osk setLayout dk
omarchy-shell ekollof.osk setRepeat 400 60
omarchy-shell ekollof.osk setRepeatEnabled off
omarchy-shell ekollof.osk toggle
```

## Security

The IPC socket (`$XDG_RUNTIME_DIR/hypr-osk.sock`) can type into the focused
session, so:

- connections are validated with `SO_PEERCRED` (same uid) plus a
  `/proc/<pid>/exe` allowlist (the Omarchy shell); everyone else is refused
- input injection (`TEXT`/`KEY`/`MOD`) only works while the keyboard is
  visible
- `PMOVE`/`PBTN` (remote pointer, debugging) require
  `HYPR_OSK_ALLOW_ANY_PEER=1` in Hyprland's environment

## Development

Edit this tree, run `./install.sh`. QML changes hot-reload (restart the
shell if a stale component cache serves old code); C++ changes need
`hyprctl plugin unload <path> && hyprctl plugin load <path>`. See
[AGENTS.md](AGENTS.md) for the architecture, the socket protocol and a
long list of hard-won gotchas.

## License

MIT for this repo's code; vendored hyprgrass keeps its upstream BSD license
(`vendor/hyprgrass/LICENSE`).
