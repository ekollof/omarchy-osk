# AGENTS.md — omarchy-osk

On-screen keyboard stack for Omarchy (Hyprland + Quickshell). Three components
plus config glue; this directory is the single source of truth. **Never edit
the deployed copies** — edit here, run `./install.sh`.

## Components

```
hypr-osk/            Hyprland compositor plugin (C++23, meson)
hypr-osk/src/main.cpp   — all of it: touch emulation, synthetic keyboard,
                          unix-socket IPC. Protocol documented in the header
                          comment of main.cpp.
hyprpm.toml          hyprpm manifest (repo root): the plugin is buildable
                     via `hyprpm add` as an alternative to install.sh
manifest.json        Omarchy shell plugin manifest (repo root): validates
                     with `omarchy plugin validate` and makes the checkout
                     `omarchy plugin add`-able (kinds: overlay + bar-widget,
                     one id for both). install.sh keeps deploying the two
                     separate plugin dirs — same ids, so don't mix paths.
vendor/hyprgrass/    Vendored hyprgrass (upstream hl-0.56.1, wf-touch
                     included): edge-gesture plugin, built + deployed by
                     install.sh like hypr-osk
shell/ekollof.osk/   Quickshell overlay plugin (the visible keyboard)
  manifest.json      — kinds: ["overlay"], keepLoaded: true
  Osk.qml            — UI, socket client, config, IPC target
  KeyboardLayout.js  — grid-driven main layer + special layer + helpers
shell/ekollof.osk-applet/   Bar widget (settings: enable, layout, rates)
  manifest.json      — kinds: ["bar-widget"], entryPoints.barWidget: Panel.qml
  Panel.qml
hypr/osk.lua         Hyprland integration: plugin autostart, hyprgrass
                     edge-swipe bind, SUPER+SHIFT+K
hypr/osk-toggle.sh   Toggle via `omarchy-shell shell summon ekollof.osk '{}'`
                     (1500 ms debounce; hyprgrass fires edge gestures twice)
install.sh           Build + deploy everything, idempotent
```

## Deployed locations (install.sh)

| Bundle                     | Deployed to                                        |
|----------------------------|----------------------------------------------------|
| `hypr-osk/build/libhypr-osk.so` | `~/.local/share/hyprland/plugins/libhypr-osk.so` |
| `vendor/hyprgrass/build/src/libhyprgrass.so` | `~/.local/share/hyprland/plugins/hyprgrass.so` |
| `hyprpm add` route         | `/var/cache/hyprpm/<user>/hypr-osk/hypr-osk.so`    |
| `shell/ekollof.osk/`       | `~/.config/omarchy/plugins/ekollof.osk/`           |
| `shell/ekollof.osk-applet/`| `~/.config/omarchy/plugins/ekollof.osk-applet/`    |
| `hypr/osk.lua`             | `~/.config/hypr/osk.lua` (required from `hyprland.lua`) |
| `hypr/osk-toggle.sh`       | `~/.config/hypr/scripts/osk-toggle.sh`             |

Supersedes `~/.local/src/hypr-osk` (old build location, retired).

## Quick start

```bash
./install.sh                 # build, deploy, register, reload
omarchy-shell ekollof.osk getState          # verify the IPC target
omarchy capture screenshot fullscreen save  # visual check (OSK open)
```

Toggle the keyboard: swipe up from the bottom screen edge, SUPER+SHIFT+K, or
the bar applet (keyboard icon, right section; left-click = settings,
right-click = toggle).

## Dev loop

1. Edit files here.
2. `./install.sh`.
3. QML changes: the shell *should* hot-reload, but a stale component cache
   frequently serves the old code — verify via the `loaded rev<N>` marker in
   `journalctl --user` (`Component.onCompleted` in Osk.qml; bump the marker
   when editing) and `omarchy restart shell` when it doesn't land.
   C++ changes: `install.sh` + `hyprctl plugin unload <path> && hyprctl plugin
   load <path>` (unload wants the full path, not the plugin name).

## Architecture / data flow

- **Touch → pointer**: the compositor plugin consumes ALL touchscreen input
  (`info.cancelled = true`) and re-emits it as a pointer: 1 finger = warp
  cursor + left button (drag = move), 2 fingers = scroll, 2-finger tap =
  right click, 3+ fingers = left for hyprgrass. Touches inside the rect the
  QML publishes via `PANEL x y w h` (normalized 0..1 against the touch
  device's monitor frame) become synthetic clicks on the keyboard layer, so
  Qt `TapHandler`s work while keyboard focus never moves (the layer is
  keyboard-focus none). Touch coords are normalized against the touch
  device's bound output, resolved per gesture from the device itself (no
  hardcoded display); `MON` answers `mon <name> x y w h` and the shell
  docks the keyboard panel on that monitor.
- **Key tap → typed char**: TapHandler → `activate()` → socket `TEXT <utf8>`
  or `KEY <evdev> 1/0` → compositor plugin maps the char against the ACTIVE
  xkb keymap of its synthetic keyboard device (`hypr-osk-vk`) and injects
  evdev keycodes + real modifier presses. Because injection flows through the
  real input pipeline, compositor keybinds and client-side composition (dead
  keys) work.
- **Layouts**: `LAYOUT <name>[(<variant>)]` recompiles the device keymap with
  xkbcommon from the system's `/usr/share/X11/xkb` — no per-layout data is
  maintained anywhere. The plugin then dumps the letter grid (`ROWS`, also
  pushed proactively on client connect and after every LAYOUT) and the QML
  renders its main layer from it. The default layout comes from the session
  locale: LC_ALL > LC_CTYPE > LANG territory → xkb layout (en_US → us,
  da_DK → dk, …), persisted on first run to `~/.config/omarchy/osk.json`.
- **Config**: `~/.config/omarchy/osk.json` — `{layout, repeatDelay,
  repeatInterval}`. The OSK plugin owns the file; the bar applet edits through
  its IPC target (`omarchy-shell ekollof.osk setLayout dk`, `setRepeat d r`,
  `getState`) and watches the file for display. Applet enable/disable toggles
  the whole `ekollof.osk` plugin via `shell.pluginRegistry.setEnabled`.

## Socket protocol (single client)

Unix socket `$XDG_RUNTIME_DIR/hypr-osk.sock`, newline-terminated lines,
replies `ok` / `err <msg>` / `PONG`; the plugin pushes `grid <json>` lines
unsolicited. Commands: `PING`, `KEY <evdev> <1|0>`, `MOD <shift|ctrl|alt|super>
<on|off>`, `MODS off`, `TEXT <utf8>`, `LAYOUT <name>[(<variant>)]`, `ROWS`,
`PANEL x y w h`, `PMOVE x y`, `PBTN <code> <1|0>`, `MON`, `CALIB` (no-op),
`STATS`. Full docs: header of `hypr-osk/src/main.cpp`.

**Access control**: the socket can type into the focused session, so peers
are validated on accept — `SO_PEERCRED` requires the same uid (always), and
unless `HYPR_OSK_ALLOW_ANY_PEER=1` is in **Hyprland's** environment at plugin
load, `/proc/<pid>/exe` must be the shell (`quickshell`; `omarchy-shell` is a
wrapper script that execs it). Everyone else gets `err unauthorized` and the
QML client is untouched. Injection commands (`TEXT`/`KEY`/`MOD`) are gated on
the published `PANEL` rect — keyboard hidden → `err hidden`. `PMOVE`/`PBTN`
are debug-only → `err pointer disabled` unless the env var is set (it also
skips the exe check; the uid check always applies).

The plugin accepts ONE client and closes the previous one (`err replaced`).
With the env var set, manual `socat` probes therefore displace the QML
client; a 2 s guard timer in Osk.qml re-announces (LAYOUT + ROWS) until the
handshake succeeds, so this self-heals. Without the var, probes are rejected
outright.

## Hard-won gotchas (do not re-learn these)

- **xkb keycodes are evdev + 8.** `xkb_keymap_*` iteration yields xkb codes;
  `IKeyboard::SKeyEvent.keycode` and the wire protocol are EVDEV codes. The
  textmap stores `key - 8` and the grid lookup adds `+ 8` — mixing them up
  shifts every key by one row-half (q types o).
- **quickshell v0.3.1 `Socket` never retries**: a failed connect keeps the
  dead QLocalSocket and `connected = true` becomes a no-op; a successful
  connect clears the internal reconnect target so server-side drops also
  stay dead. The Socket therefore lives in a `Loader` and is recreated by a
  retry timer on `error` / unexpected `onConnectedChanged(false)`.
- **Unix-socket connects can complete synchronously during component
  creation** — handlers fire before the Loader's `item` is assigned and
  before the config FileView loads. Osk.qml gates its handshake on
  `cfgLoaded && announced` flags + the 2 s guard timer. Do not send from
  `onConnectedChanged` directly.
- **`Socket.write()` needs `flush()`** and silently no-ops when
  disconnected. All writes go through Osk.qml's `send()`.
- **Plugin unload is by full .so path**, not plugin name (`hyprctl plugin
  unload <path>`); loading twice by path errors with "Cannot load a plugin
  twice!" even under a different path.
- **The shell's generic `shell call <id> <method>` verb is broken** (panel
  loaders resolve wrong). Plugins expose their own `IpcHandler` target
  instead — `omarchy-shell ekollof.osk <method> [args…]`.
- **Component cache staleness**: after editing plugin QML, `rescanPlugins`
  may keep serving the old component. Bump the `rev<N>` marker in
  Osk.qml's `Component.onCompleted` and `omarchy restart shell` until the
  new marker shows in the journal.
- **`PLUGIN_EXIT` ordering matters** (thread join → bus disconnect → timer
  removal → key release → device destroy): the .so is unmapped after unload
  and anything still referencing it crashes the compositor. Preserve the
  teardown order in main.cpp.
- **Calling input/monitor code from touch event callbacks deadlocks the
  input pipeline** — handlers only record state and schedule the zero-time
  apply timer; all compositor mutations happen in `applyTouches()` on the
  idle phase of the main thread. Same for socket commands: queued into a
  fixed ring buffer, drained by an event-loop timer on the main thread.
- **The touch resolver must see every contact down.** `touchDown` sets
  `down_flag` unconditionally and the `applyTouches()` down-branch is a mode
  resolver driven by (fingers, pressed) — the second finger's down is what
  enters two-finger scroll. An earlier version flagged only the first
  finger's down, so scroll/right-click could never engage while taps and
  drags (first finger only) kept working. Batched landings (both fingers
  before the apply timer) must resolve in a single pass. Scroll is emitted
  touchpad-style (raw logical px, 1:1 with the hand) and shaped per gesture
  by focused window class: Hyprland 0.56 only forwards v120 for wheel
  source, and Chromium rescales plain axis values by 1/10 * 120 (wheel
  ticks) — so chrom*-class windows get SOURCE_WHEEL + value120 (exact
  pixels via Chromium's v8 handler, which replaces the legacy delta in the
  same frame), while kitty & co read plain axis as continuous pixels (v120
  would mean wheel notches to them) and get SOURCE_FINGER. Finger #1's
  button-down is deferred ~130 ms (`PRESS_DELAY_MS`) so a landing second
  finger cancels it — two-finger scroll never drag-selects text; quick taps
  click on lift, held single-finger drags get the button after the delay.
- **Bar widget popup**: use the shell's `qs.Ui` `Panel` base + `BarIconButton`
  + `KeyboardPanel` (see `shell/ekollof.osk-applet/Panel.qml` and
  `~/src/omarchy/shell/plugins/panels/power/Panel.qml` as the canonical
  example). `bar.shell` is the shell root; `bar.run(cmd)` execs detached.
- **Row sizing**: keyboard rows flex per-row (`keyRow.rowUnit` divides panel
  width minus padding and gaps by the row's own unit sum). A fixed divisor
  overflows: grid rows are 14–15 units wide on a 15-unit screen → Esc slides
  off-screen.
- **Nerd Font glyphs** in QML: insert via `python3 -c "print(chr(0xf11c))"`
  — editing tools can strip multi-byte codepoints.

## Testing checklist

1. `./install.sh` — exits 0, "compositor plugin: loaded".
2. `omarchy-shell ekollof.osk getState` → JSON with `gridLoaded: true`.
3. Toggle: SUPER+SHIFT+K, edge swipe, bar applet right-click → layer
   `ekollof-osk` appears/disappears in `hyprctl layers`.
4. Typing: summon over a text field, tap keys — output matches labels; test a
   non-US layout (`omarchy-shell ekollof.osk setLayout dk` → æ ø å type and
   render), then back (`setLayout us`).
5. Touch: tap = left click under finger, drag = move, two-finger drag =
   scroll, two-finger tap = right click, keys on the OSK panel = taps.
6. Hold backspace/arrows → key repeat at configured delay/rate.
7. Bar applet: layout picker lists `localectl list-x11-keymap-layouts`;
   sliders persist to `~/.config/omarchy/osk.json`.
8. Visual: `omarchy capture screenshot fullscreen save` (avoid bare
   `omarchy capture screenshot` — interactive, blocks non-interactive shells).
   Check row fit, then delete the png.
9. Security: without the env var, `socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr-osk.sock`
   → `err unauthorized` and the QML client survives (`getState` still shows
   `gridLoaded: true`). Keyboard hidden: `TEXT hi` → `err hidden` (open it:
   it types). `PMOVE 0 0` → `err pointer disabled` unless
   `HYPR_OSK_ALLOW_ANY_PEER=1` is set in Hyprland's env.
10. `STATS` over the socket shows `layout=<spec> textmap=<n> inject=<0|1>
    anypeer=<0|1>`; bad layout: `LAYOUT nonsense99` → `err bad layout`
    (validated client-side of the queue with a pre-compile; compositor never
    sees it). Socket probes require the env var (peer check rejects them
    otherwise).

## Dependencies

meson, ninja, pkg-config, hyprland headers (`dependency('hyprland')` —
matches the RUNNING Hyprland version; rebuild after Hyprland updates),
pixman, libinput, wayland-server, xkbcommon (≥ 1.0 for
`xkb_keymap_key_get_mods_for_level`), libdrm; quickshell ≥ 0.3 (omarchy-shell),
localectl (layout list). All ship with Omarchy.

**hyprgrass** (edge gesture) is vendored at `vendor/hyprgrass/` — upstream
horriblename/hyprgrass pinned to tag `hl-0.56.1` (commit `1a8e258f`, wf-touch
submodule included, so builds are offline-capable). Rationale: hyprgrass
HEAD already requires a newer Hyprland than 0.56.x (it includes
`hyprland/src/keybinds/Manager.hpp`, absent from 0.56.2 headers) and its
hyprpm commit_pins had no 0.56.2 entry, so the hyprpm/AUR routes either
fail or chase upstream. install.sh builds the vendored copy exactly like
hypr-osk and deploys `~/.local/share/hyprland/plugins/hyprgrass.so` (skipped
when `hyprpm list` reports hyprgrass). Re-vendoring for a future Hyprland:
clone upstream, checkout the matching `hl-<version>` tag, rsync over
vendor/hyprgrass/ (minus .git, build, subprojects/.wraplock), verify
`meson setup` + compile against system headers, commit. The version gap
shows up as a build failure (missing hyprland headers), not a runtime crash.
Without hyprgrass only the edge-swipe gesture is lost; the keyboard still
toggles via SUPER+SHIFT+K and the bar applet.

**hyprpm route for hypr-osk itself**: `hyprpm.toml` (repo root) makes the
compositor plugin hyprpm-buildable: `hyprpm update` once (headers build;
needs interactive sudo plus cmake, cpio, hyprwayland-scanner, gcc, g++,
pkg-config, git), then `hyprpm add <repo-url-or-path>` + `hyprpm enable
hypr-osk`. hyprpm prepends its headers to `PKG_CONFIG_PATH`, so the same
meson.build serves both routes. The .so lands at
`/var/cache/hyprpm/<user>/hypr-osk/hypr-osk.so`; osk.lua loads whichever
copy exists at session start (flat install.sh deploy preferred; both
loaders guard against double-loads by plugin name). install.sh skips its
own build when `hyprpm list` reports hypr-osk (same guard for hyprgrass).
The manifest is validated against hyprpm's CManifest source and the route
has been exercised live (headers build via `hyprpm update` needs
interactive sudo). The PRIMARY path is install.sh's local build; to return
a hyprpm-managed machine to it, run `hyprpm remove hypr-osk` in a
terminal.
