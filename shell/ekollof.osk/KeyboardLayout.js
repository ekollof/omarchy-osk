// Layout generation for the on-screen keyboard.
//
// The main layer is GENERATED from the active xkb keymap: the hypr-osk
// compositor plugin dumps the letter grid of the current layout (ROWS
// command) — evdev keycode order is the physical arrangement for every
// latin layout, so any layout installed under /usr/share/X11/xkb works
// with no per-layout data here. Only functional keys and the special
// layer are defined by hand.
//
// Grid key shape from the plugin: { l, s, c, raw }
//   l = level-0 label, s = level-1 (shift) label, c = evdev keycode,
//   raw = true for dead keys etc. (send KEY <c>, not TEXT)
//
// Key types in the returned rows:
//   char  — sent as TEXT (the plugin resolves keycode+mods via the active keymap)
//   code  — raw evdev keycode, sent as KEY <c> 1 / KEY <c> 0
//   mod   — sticky modifier held by the plugin (MOD <m> on/off)
//   shift — QML-side one-shot shift
//   caps  — QML-side shift lock
//   layer — switch layer / back
//
// char and code keys auto-repeat on hold (mod/shift/caps/layer keys don't);
// the old r: hold-repeat marker is no longer consulted.

var functionalRow = [
  { l: "⌨͕", w: 1, t: "layer", to: "special", scheme: 1 },
  { l: "Ctr", w: 1, t: "mod", m: "ctrl", scheme: 1 },
  { l: "Alt", w: 1, t: "mod", m: "alt", scheme: 1 },
  { l: "Sup", w: 1, t: "mod", m: "super", scheme: 1 },
  { l: "␣", w: 3.5, t: "code", c: 57 },
  { l: "`", s: "~", w: 0.75, t: "char" },
  { l: "←", w: 0.75, t: "code", c: 105, scheme: 1, r: 1 },
  { l: "↓", w: 0.75, t: "code", c: 108, scheme: 1, r: 1 },
  { l: "↑", w: 0.75, t: "code", c: 103, scheme: 1, r: 1 },
  { l: "→", w: 0.75, t: "code", c: 106, scheme: 1, r: 1 }
]

// Shown until the first grid arrives from the plugin (US arrangement).
var mainFallback = [
  [
    { l: "Esc", w: 0.75, t: "code", c: 1, scheme: 1 },
    { l: "1", s: "!", w: 1, t: "char" },
    { l: "2", s: "@", w: 1, t: "char" },
    { l: "3", s: "#", w: 1, t: "char" },
    { l: "4", s: "$", w: 1, t: "char" },
    { l: "5", s: "%", w: 1, t: "char" },
    { l: "6", s: "^", w: 1, t: "char" },
    { l: "7", s: "&", w: 1, t: "char" },
    { l: "8", s: "*", w: 1, t: "char" },
    { l: "9", s: "(", w: 1, t: "char" },
    { l: "0", s: ")", w: 1, t: "char" },
    { l: "-", s: "_", w: 1, t: "char" },
    { l: "=", s: "+", w: 1, t: "char" },
    { l: "`", s: "~", w: 1, t: "char" },
    { l: "⌫", w: 1.5, t: "code", c: 14, scheme: 1, r: 1 }
  ],
  [
    { l: "Tab", w: 1.25, t: "code", c: 15, scheme: 1 },
    { l: "q", s: "Q", w: 1, t: "char" },
    { l: "w", s: "W", w: 1, t: "char" },
    { l: "e", s: "E", w: 1, t: "char" },
    { l: "r", s: "R", w: 1, t: "char" },
    { l: "t", s: "T", w: 1, t: "char" },
    { l: "y", s: "Y", w: 1, t: "char" },
    { l: "u", s: "U", w: 1, t: "char" },
    { l: "i", s: "I", w: 1, t: "char" },
    { l: "o", s: "O", w: 1, t: "char" },
    { l: "p", s: "P", w: 1, t: "char" },
    { l: "[", s: "{", w: 1, t: "char" },
    { l: "]", s: "}", w: 1, t: "char" },
    { l: "\\", s: "|", w: 1, t: "char" }
  ],
  [
    { l: "⇪", w: 1.25, t: "caps", scheme: 1 },
    { l: "a", s: "A", w: 1, t: "char" },
    { l: "s", s: "S", w: 1, t: "char" },
    { l: "d", s: "D", w: 1, t: "char" },
    { l: "f", s: "F", w: 1, t: "char" },
    { l: "g", s: "G", w: 1, t: "char" },
    { l: "h", s: "H", w: 1, t: "char" },
    { l: "j", s: "J", w: 1, t: "char" },
    { l: "k", s: "K", w: 1, t: "char" },
    { l: "l", s: "L", w: 1, t: "char" },
    { l: ";", s: ":", w: 1, t: "char" },
    { l: "'", s: "\"", w: 1, t: "char" },
    { l: "Enter", w: 1.75, t: "code", c: 28, scheme: 1 }
  ],
  [
    { l: "⇧", w: 1.5, t: "shift", scheme: 1 },
    { l: "z", s: "Z", w: 1, t: "char" },
    { l: "x", s: "X", w: 1, t: "char" },
    { l: "c", s: "C", w: 1, t: "char" },
    { l: "v", s: "V", w: 1, t: "char" },
    { l: "b", s: "B", w: 1, t: "char" },
    { l: "n", s: "N", w: 1, t: "char" },
    { l: "m", s: "M", w: 1, t: "char" },
    { l: ",", s: "<", w: 1, t: "char" },
    { l: ".", s: ">", w: 1, t: "char" },
    { l: "/", s: "?", w: 1, t: "char" },
    { l: "⇧", w: 1.5, t: "shift", scheme: 1 }
  ],
  functionalRow
]

function gridKey(k) {
  if (k.raw)
    return { l: k.l || "◇", w: 1, t: "code", c: k.c, scheme: 1 }
  return { l: k.l, s: k.s, w: 1, t: "char" }
}

function buildMain(grid) {
  const rows = grid && grid.rows
  if (!rows || rows.length < 4)
    return mainFallback
  return [
    [{ l: "Esc", w: 0.75, t: "code", c: 1, scheme: 1 }].concat(
      rows[0].map(gridKey),
      [{ l: "⌫", w: 1.5, t: "code", c: 14, scheme: 1, r: 1 }]
    ),
    [{ l: "Tab", w: 1.25, t: "code", c: 15, scheme: 1 }].concat(rows[1].map(gridKey)),
    [{ l: "⇪", w: 1.25, t: "caps", scheme: 1 }].concat(
      rows[2].map(gridKey),
      [{ l: "Enter", w: 1.75, t: "code", c: 28, scheme: 1 }]
    ),
    [{ l: "⇧", w: 1.5, t: "shift", scheme: 1 }].concat(
      rows[3].map(gridKey), // ISO KEY_102ND only when it adds a glyph (de, gb, … — not us)
      [{ l: "⇧", w: 1.5, t: "shift", scheme: 1 }]
    ),
    functionalRow
  ]
}

var special = [
  [
    { l: "Esc", w: 1, t: "code", c: 1, scheme: 1 },
    { l: "Alt", w: 1, t: "mod", m: "alt", scheme: 1 },
    { l: "↑", w: 1, t: "code", c: 103, scheme: 1, r: 1 },
    { l: "↓", w: 1, t: "code", c: 108, scheme: 1, r: 1 },
    { l: "←", w: 1, t: "code", c: 105, scheme: 1, r: 1 },
    { l: "→", w: 1, t: "code", c: 106, scheme: 1, r: 1 },
    { l: "⇈", w: 1, t: "code", c: 104, scheme: 1, r: 1 },
    { l: "⇊", w: 1, t: "code", c: 109, scheme: 1, r: 1 },
    { l: "⇤", w: 1, t: "code", c: 102, scheme: 1, r: 1 },
    { l: "⇥", w: 1, t: "code", c: 107, scheme: 1, r: 1 }
  ],
  [
    { l: "1", s: "!", w: 1, t: "char" },
    { l: "2", s: "@", w: 1, t: "char" },
    { l: "3", s: "#", w: 1, t: "char" },
    { l: "4", s: "$", w: 1, t: "char" },
    { l: "5", s: "%", w: 1, t: "char" },
    { l: "6", s: "^", w: 1, t: "char" },
    { l: "7", s: "&", w: 1, t: "char" },
    { l: "8", s: "*", w: 1, t: "char" },
    { l: "9", s: "(", w: 1, t: "char" },
    { l: "0", s: ")", w: 1, t: "char" }
  ],
  [
    { l: "⇪", w: 1, t: "caps", scheme: 1 },
    { l: "Sup", w: 1, t: "mod", m: "super", scheme: 1 },
    { l: "`", s: "~", w: 1, t: "char" },
    { l: "'", s: "\"", w: 1, t: "char" },
    { l: "-", s: "_", w: 1, t: "char" },
    { l: "=", s: "+", w: 1, t: "char" },
    { l: "[", s: "{", w: 1, t: "char" },
    { l: "]", s: "}", w: 1, t: "char" },
    { l: "\\", s: "|", w: 1, t: "char" },
    { l: "Del", w: 1, t: "code", c: 111, scheme: 1 }
  ],
  [
    { l: "⇧", w: 2, t: "shift", scheme: 1 },
    { l: ";", s: ":", w: 1, t: "char" },
    { l: "/", s: "?", w: 1, t: "char" },
    { l: "<", w: 1, t: "char" },
    { l: ">", w: 1, t: "char" },
    { l: "⌫", w: 1, t: "code", c: 14, scheme: 1, r: 1 }
  ],
  [
    { l: "Abc", w: 1, t: "back", scheme: 1 },
    { l: ",", s: "<", w: 1, t: "char" },
    { l: "␣", w: 4, t: "code", c: 57 },
    { l: ".", s: ">", w: 1, t: "char" },
    { l: "Enter", w: 2, t: "code", c: 28, scheme: 1 }
  ]
]

function rows(layer, grid) {
  return layer === "special" ? special : buildMain(grid)
}
