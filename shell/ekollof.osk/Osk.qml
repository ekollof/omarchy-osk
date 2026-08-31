import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.Commons
import "KeyboardLayout.js" as KeyboardLayout

// On-screen keyboard (wvkbd terminal layout) as an omarchy shell overlay.
//
// Touch handling: the hypr-osk compositor plugin consumes all touchscreen
// input EXCEPT inside the rect we publish via the PANEL command — inside it,
// the plugin re-emits the contact as a synthetic pointer click on this layer
// surface (TapHandler works; keyboard focus never moves here). Key presses
// go back over the same unix socket as TEXT/KEY/MOD commands and are
// injected into the focused client by the plugin.
//
// Layouts: the plugin's synthetic keyboard carries the ACTIVE xkb keymap
// (LAYOUT command, any layout installed under /usr/share/X11/xkb). The main
// layer's letter grid is dumped from that keymap (ROWS command) so labels
// always match what typing produces. Config (layout + repeat rates) lives
// in ~/.config/omarchy/osk.json; the bar applet edits it via the IPC methods
// below (getState/setLayout/setRepeat).
//
// Summon = toggle: `omarchy-shell shell summon ekollof.osk '{}'` opens when
// hidden and hides when open.
Item {
  id: root

  // Injected by omarchy-shell
  property var shell: null
  property var manifest: null

  property bool opened: false
  property string currentLayer: "main"
  property bool shift: false          // one-shot: consumed by the next char
  property bool capsLock: false       // sticky upper
  property var stickyMods: ({ "ctrl": false, "alt": false, "super": false })

  // Settings, persisted in ~/.config/omarchy/osk.json
  property string layout: ""          // "us", "dk", "us(intl)"…
  property int repeatDelay: 400       // ms before hold-repeat starts
  property int repeatInterval: 60     // ms between repeats
  property var grid: null             // letter grid from the plugin (ROWS)

  readonly property real unit: (panel.width - Style.space(8) * 2) / 15.0 // widest row (home row)
  readonly property int keyH: Style.space(46)
  readonly property int panelH: rowsCol.implicitHeight + Style.space(8) * 2

  function open(payloadJson) {
    if (root.opened)
      root.close()
    else
      root.show()
  }

  function close() {
    root.hide()
  }

  function ping() { return "ok" }

  function show() {
    root.opened = true
    syncPanel()
  }

  function hide() {
    root.opened = false
    root.shift = false
    root.capsLock = false
    setLayer("main")
    send("PANEL 0 0 0 0")
    send("MODS off")
  }

  function setLayer(name) {
    root.currentLayer = name
    if (name !== "main") {
      // entering a layer drops one-shot shift, keeps sticky mods
      root.shift = false
    }
  }

  function syncPanel() {
    // Publish our rect to the plugin, normalized against the touch screen's
    // logical frame (the same frame ev.pos is normalized to; MON is the
    // plugin's own view of that frame — 1280x800 for the transformed panel).
    const s = panel.screen
    if (!root.opened || !s || s.height <= 0)
      return
    // root.panelH, not panel.height: the window's reported height lags the
    // mapped layer surface (258 vs 101 observed)
    const ny = (s.height - root.panelH) / s.height
    const nh = root.panelH / s.height
    console.log("[ekollof.osk] syncPanel panelH=" + root.panelH + " sh=" + s.height +
                " ny=" + ny.toFixed(4) + " sock=" + connected())
    send("PANEL 0 " + ny.toFixed(4) + " 1 " + nh.toFixed(4))
  }

  // ---- settings -----------------------------------------------------------

  // Locale of the session decides the default layout: da_DK.UTF-8 → "dk".
  // Standard precedence LC_ALL > LC_CTYPE > LANG; territory → xkb layout.
  function defaultLayout() {
    const loc = String(Quickshell.env("LC_ALL") || Quickshell.env("LC_CTYPE") || Quickshell.env("LANG") || "")
    const m = loc.match(/^[a-zA-Z]{2,3}[_-]([a-zA-Z]{2,4})/)
    const territory = m ? m[1].toUpperCase() : ""
    const map = {
      US: "us", GB: "gb", IE: "ie", CA: "ca", AU: "us", NZ: "us", ZA: "za",
      DK: "dk", DE: "de", AT: "at", CH: "ch", ES: "es", MX: "latam", AR: "latam",
      FR: "fr", BE: "be", IT: "it", PT: "pt", BR: "br", NL: "nl", LU: "lu",
      NO: "no", SE: "se", FI: "fi", IS: "is", PL: "pl", CZ: "cz", SK: "sk",
      HU: "hu", SI: "si", HR: "hr", RS: "rs", BA: "ba", TR: "tr", GR: "gr",
      RU: "ru", UA: "ua", BY: "by", RO: "ro", BG: "bg", EE: "ee", LV: "lv",
      LT: "lt", JP: "jp", KR: "kr", CN: "cn", TW: "tw", HK: "hk", IN: "in",
      PK: "pk", IL: "il", SA: "sa", AE: "ae", TH: "th", VN: "vn", ID: "id", MY: "my"
    }
    return map[territory] || "us"
  }

  function clampInt(v, min, max, fallback) {
    const n = parseInt(v, 10)
    if (!isFinite(n))
      return fallback
    return Math.min(max, Math.max(min, n))
  }

  function applyConfig(raw) {
    let cfg = {}
    try { cfg = JSON.parse(raw || "{}") || {} } catch (e) { cfg = {} }
    let layout = String(cfg.layout || "")
    if (!/^[a-z0-9_]+(\([a-z0-9_]+\))?$/.test(layout))
      layout = defaultLayout()
    root.layout = layout
    root.repeatDelay = clampInt(cfg.repeatDelay, 100, 2000, 400)
    root.repeatInterval = clampInt(cfg.repeatInterval, 15, 500, 60)
    root.cfgLoaded = true
    // persist on first run so the applet sees the LANG-derived default too
    if (!raw || !cfg.layout || cfg.repeatDelay !== root.repeatDelay || cfg.repeatInterval !== root.repeatInterval)
      persistConfig()
    // pushes the (possibly changed) layout to the plugin and pulls the grid
    Qt.callLater(root.announce)
  }

  function persistConfig() {
    cfgFile.setText(JSON.stringify({
      layout: root.layout,
      repeatDelay: root.repeatDelay,
      repeatInterval: root.repeatInterval
    }) + "\n")
  }

  // ---- IPC methods for the bar applet (omarchy-shell shell call …) --------

  function getState() {
    return JSON.stringify({
      layout: root.layout,
      repeatDelay: root.repeatDelay,
      repeatInterval: root.repeatInterval,
      gridLoaded: !!root.grid
    })
  }

  function setLayout(arg) {
    const layout = String(arg || "").trim()
    if (!/^[a-z0-9_]+(\([a-z0-9_]+\))?$/.test(layout)) {
      console.warn("[ekollof.osk] setLayout: bad layout " + layout)
      return "err bad layout"
    }
    root.layout = layout
    root.announced = false // layout changed: re-handshake
    persistConfig()
    Qt.callLater(root.announce)
    return "ok"
  }

  function setRepeat(arg) {
    const parts = String(arg || "").trim().split(/[\s,]+/)
    if (parts.length !== 2)
      return "err need 'delay interval'"
    root.repeatDelay = clampInt(parts[0], 100, 2000, root.repeatDelay)
    root.repeatInterval = clampInt(parts[1], 15, 500, root.repeatInterval)
    persistConfig()
    return "ok"
  }

  // ---- IPC: the bar applet (and scripts) drive settings through here -----
  // `omarchy-shell ekollof.osk <method> [args…]`. The shell target's generic
  // `call` verb is currently broken for panel plugins, so the applet routes
  // through this dedicated target instead.
  IpcHandler {
    target: "ekollof.osk"

    function ping(): string { return "pong" }

    function getState(): string { return root.getState() }

    function setLayout(layout: string): string { return root.setLayout(layout) }

    function setRepeat(delay: string, interval: string): string {
      return root.setRepeat(delay + " " + interval)
    }

    function toggle(): string {
      root.open("{}")
      return root.opened ? "open" : "closed"
    }
  }

  // ---- plugin socket ------------------------------------------------------
  // On unix sockets connect() can complete synchronously DURING component
  // creation, so signals may fire before the Loader's item is assigned and
  // before the config file loads. announce() only fires once BOTH sides are
  // ready, and re-arms until it succeeds; the plugin also pushes the grid
  // on every client connect, so a lost announce self-heals anyway.
  property bool sockReady: false
  property bool cfgLoaded: false
  property bool announced: false

  function connected() {
    const s = sockLoader.item
    return !!(s && s.connected)
  }

  function send(line) {
    const s = sockLoader.item
    if (s && s.connected) {
      s.write(line + "\n")
      s.flush() // write() queues; flush puts it on the wire now
    } else {
      console.warn("[ekollof.osk] socket down, dropping: " + line)
    }
  }

  // Handshake: layout first, then the plugin replies with the grid (and
  // pushes a fresh one after every LAYOUT)
  function announce() {
    if (!root.cfgLoaded || root.announced || !connected())
      return
    root.announced = true
    send("LAYOUT " + root.layout)
    send("ROWS")
    if (root.opened)
      Qt.callLater(root.syncPanel)
  }

  function handleReply(line) {
    if (line.indexOf("grid ") === 0) {
      try {
        root.grid = JSON.parse(line.substring(5))
      } catch (e) {
        console.warn("[ekollof.osk] bad grid reply: " + e)
      }
    }
  }

  Loader {
    id: sockLoader
    active: true
    sourceComponent: Socket {
      path: Quickshell.env("XDG_RUNTIME_DIR") + "/hypr-osk.sock"
      connected: true
      parser: SplitParser {
        onRead: function(line) { root.handleReply(line) }
      }
      onConnectedChanged: {
        console.log("[ekollof.osk] socket " + (connected ? "connected" : "disconnected"))
        root.sockReady = connected
        if (!connected) {
          root.announced = false
          retryTimer.restart() // server dropped us; quickshell won't redial on its own
        } else {
          Qt.callLater(root.announce)
        }
      }
      onError: retryTimer.restart()
    }
  }

  // Safety net for any missed announce (races between file load, connects
  // and layout switches): retry every 2s until the handshake is through
  Timer {
    interval: 2000
    running: true
    repeat: true
    onTriggered: root.announce()
  }

  Timer {
    id: retryTimer
    interval: 1500
    onTriggered: {
      // a fresh Socket object is the only way to retry a failed connect
      sockLoader.active = false
      sockLoader.active = true
    }
  }

  // ---- config file --------------------------------------------------------

  function cfgPath() {
    const base = Quickshell.env("XDG_CONFIG_HOME") || (Quickshell.env("HOME") + "/.config")
    return base + "/omarchy/osk.json"
  }

  FileView {
    id: cfgFile
    path: root.cfgPath()
    watchChanges: true
    atomicWrites: true
    printErrors: false
    blockLoading: true
    onLoaded: root.applyConfig(text())
    onLoadFailed: root.applyConfig("") // missing file → LANG-derived defaults
    onFileChanged: reload()
  }

  // press-and-hold auto-repeat (backspace, arrows) — one key held at a time
  // on touch, so a single root timer works for every key
  function startRepeat(code, key) {
    repeatTimer.code = code
    repeatTimer.key = key
    repeatTimer.fired = false
    repeatTimer.interval = Math.max(100, root.repeatDelay)
    repeatTimer.restart()
  }

  function stopRepeat() {
    repeatTimer.stop()
  }

  Timer {
    id: repeatTimer
    interval: 400
    property int code: 0
    property bool fired: false
    property QtObject key: null
    onTriggered: {
      if (!fired) {
        fired = true
        interval = Math.max(15, root.repeatInterval)
      }
      if (key)
        key.repeatFired = true
      send("KEY " + code + " 1")
      send("KEY " + code + " 0")
    }
  }

  function sendMod(name, on) {
    send("MOD " + name + (on ? " on" : " off"))
  }

  function clearOneShot() {
    // phone-style: shift and held mods apply to exactly one key, then clear
    let changed = false
    for (const m in root.stickyMods) {
      if (root.stickyMods[m]) {
        root.stickyMods[m] = false
        sendMod(m, false)
        changed = true
      }
    }
    if (changed)
      root.stickyModsChanged()
    if (root.shift)
      root.shift = false
  }

  function activate(k) {
    console.log("[ekollof.osk] key t=" + k.t + " k=" + (k.l || "") + " c=" + (k.c || 0) +
                " m=" + (k.m || "") + " sock=" + connected())
    switch (k.t) {
    case "char": {
      // caps lock shifts letters only; caps+shift gives lowercase (real caps)
      const isLetter = /^[a-z]$/.test(k.l || "")
      let ch = k.l
      const wantUpper = k.s && (root.shift ? (root.capsLock ? false : true) : (root.capsLock && isLetter))
      if (wantUpper)
        ch = k.s
      else if (root.shift && root.capsLock && isLetter && k.s)
        ch = k.l
      send("TEXT " + ch)
      clearOneShot()
      break
    }
    case "code":
      send("KEY " + k.c + " 1")
      send("KEY " + k.c + " 0")
      clearOneShot()
      break
    case "mod": {
      root.stickyMods[k.m] = !root.stickyMods[k.m]
      root.stickyModsChanged()
      sendMod(k.m, root.stickyMods[k.m])
      break
    }
    case "shift":
      root.shift = !root.shift
      break
    case "caps":
      root.capsLock = !root.capsLock
      if (root.capsLock)
        root.shift = false
      break
    case "layer":
      setLayer(k.to)
      break
    case "back":
      setLayer("main")
      break
    }
  }

  // ---- window ------------------------------------------------------------
  PanelWindow {
    id: panel
    visible: root.opened
    screen: Quickshell.screens.find(s => s.name === "eDP-1") ?? null
    anchors {
      left: true
      bottom: true
      right: true
    }
    exclusiveZone: root.panelH
    color: "transparent"
    WlrLayershell.namespace: "ekollof-osk"
    WlrLayershell.layer: WlrLayer.Top
    // Never take keyboard focus: taps must keep focus on the target window —
    // the plugin injects TEXT/KEY into the currently focused client. This is
    // how wvkbd behaves too (keyboard_interactivity = NONE).
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

    implicitHeight: root.panelH

    onScreenChanged: root.syncPanel()
    onHeightChanged: root.syncPanel()
    onWidthChanged: root.syncPanel()

    Rectangle {
      id: kbdColumn
      anchors.fill: parent
      color: Color.bar.background

      Column {
        id: rowsCol
        anchors.centerIn: parent
        spacing: Style.space(3)

        Repeater {
          model: KeyboardLayout.rows(root.currentLayer, root.grid)

          Row {
            id: keyRow
            required property var modelData
            spacing: Style.space(3)

            // per-row unit: every row flexes to span the panel width exactly,
            // gaps included (rows differ in key count and width units)
            readonly property real totalUnits: {
              let s = 0
              for (let i = 0; i < modelData.length; i++)
                s += modelData[i].w
              return s
            }
            readonly property real rowUnit: (kbdColumn.width - Style.space(8) * 2 -
                                             (modelData.length - 1) * spacing) / totalUnits

            Repeater {
              model: keyRow.modelData

              Rectangle {
                id: key
                required property var modelData
                property bool keyDown: false
                property bool repeatFired: false // hold-repeat owned this press; suppress the tap
                readonly property bool upper: root.shift || root.capsLock
                readonly property bool active: keyDown ||
                                               (modelData.t === "shift" && root.shift) ||
                                               (modelData.t === "caps" && root.capsLock) ||
                                               (modelData.t === "mod" && root.stickyMods[modelData.m])

                width: modelData.w * keyRow.rowUnit
                height: root.keyH
                radius: Style.space(5)
                color: key.active ? Color.bar.active : (modelData.scheme ? Qt.alpha(Color.bar.text, 0.14) : Qt.alpha(Color.bar.text, 0.06))
                border.width: modelData.scheme ? 1 : 0
                border.color: Qt.alpha(Color.bar.text, 0.25)

                Text {
                  anchors.centerIn: parent
                  text: (key.upper && parent.modelData.s) ? parent.modelData.s : parent.modelData.l
                  color: key.active ? Color.bar.background : Color.bar.text
                  font.family: Style.fontFamily
                  font.pixelSize: parent.modelData.l.length > 2 ? Style.space(15) : Style.space(20)
                }

                TapHandler {
                  onPressedChanged: {
                    key.keyDown = pressed
                    if (pressed) {
                      key.repeatFired = false
                      if (key.modelData.r)
                        root.startRepeat(key.modelData.c, key)
                    } else {
                      root.stopRepeat()
                    }
                  }
                  onTapped: {
                    if (!key.repeatFired)
                      root.activate(key.modelData)
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  onOpenedChanged: syncPanel()
  onPanelHChanged: syncPanel()
  Component.onCompleted: console.log("[ekollof.osk] loaded rev4 layout=" + root.layout +
                                     " cfg=" + root.cfgPath())
}
