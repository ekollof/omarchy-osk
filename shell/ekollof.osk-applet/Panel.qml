import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Bar applet for the ekollof.osk on-screen keyboard plugin: enable/disable
// (plugin state in shell.json), show/hide (shell toggle), layout picker
// (xkb layouts via localectl, applied over the hypr-osk socket by the OSK
// plugin) and key-repeat rate sliders. Settings live in the OSK plugin's
// ~/.config/omarchy/osk.json — this applet only reads it; writes go through
// the OSK's IPC methods so it stays the single source of truth.
Panel {
  id: root
  moduleName: "ekollof.osk-applet"
  ipcTarget: "ekollof.osk-applet"

  readonly property string oskPluginId: "ekollof.osk"
  readonly property string kbGlyph: ""

  property bool oskEnabled: false
  property string layout: "us"
  property bool repeatEnabled: true
  property int repeatDelay: 400
  property int repeatInterval: 60
  property var layouts: []

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  function shellObj() {
    return root.bar && root.bar.shell ? root.bar.shell : null
  }

  function refreshState() {
    const shell = root.shellObj()
    if (shell && shell.pluginRegistry && typeof shell.pluginRegistry.isEnabled === "function")
      root.oskEnabled = shell.pluginRegistry.isEnabled(root.oskPluginId) === true
  }

  function setOskEnabled(on) {
    const shell = root.shellObj()
    let ok = false
    if (shell && shell.pluginRegistry && typeof shell.pluginRegistry.setEnabled === "function")
      ok = shell.pluginRegistry.setEnabled(root.oskPluginId, on) === true
    if (!ok && root.bar)
      root.bar.run("omarchy plugin " + (on ? "enable" : "disable") + " " + root.oskPluginId)
    root.oskEnabled = on
  }

  function toggleOsk() {
    const shell = root.shellObj()
    if (shell && typeof shell.toggle === "function")
      shell.toggle(root.oskPluginId, "{}")
    else if (root.bar)
      root.bar.run("omarchy-shell shell toggle " + root.oskPluginId + " '{}'")
  }

  // Settings changes ride the OSK plugin's own IPC target.
  function callOsk(method, arg) {
    if (root.bar)
      root.bar.run("omarchy-shell ekollof.osk " + method + " " + arg)
  }

  function applyConfig(raw) {
    let cfg = {}
    try { cfg = JSON.parse(raw || "{}") || {} } catch (e) { cfg = {} }
    if (cfg.layout)
      root.layout = String(cfg.layout)
    if (cfg.repeat !== undefined)
      root.repeatEnabled = !!cfg.repeat
    if (cfg.repeatDelay)
      root.repeatDelay = parseInt(cfg.repeatDelay, 10) || 400
    if (cfg.repeatInterval)
      root.repeatInterval = parseInt(cfg.repeatInterval, 10) || 60
  }

  function setLayouts(raw) {
    const list = String(raw || "").split("\n").map(s => s.trim()).filter(s => s !== "")
    root.layouts = list.length > 0 ? list : ["us"]
  }

  onOpenedChanged: {
    if (opened) {
      refreshState()
      if (root.layouts.length === 0)
        layoutsProc.running = true
    }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: root.kbGlyph
    tooltipText: "On-screen keyboard"
    onPressed: function(b) {
      if (b === Qt.RightButton)
        root.toggleOsk()
      else
        root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(column.implicitHeight)

    Column {
      id: column
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.top: parent.top
      spacing: Style.space(14)

      // ---------- hero ----------
      Row {
        spacing: Style.space(12)

        Text {
          textFormat: Text.PlainText
          text: root.kbGlyph
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.display
          anchors.verticalCenter: parent.verticalCenter
        }

        Column {
          anchors.verticalCenter: parent.verticalCenter
          spacing: Style.space(2)

          Text {
            text: "On-screen keyboard"
            color: root.bar ? root.bar.foreground : Color.foreground
            font.family: root.bar ? root.bar.fontFamily : Style.font.family
            font.pixelSize: Style.font.title
            font.bold: true
          }

          Text {
            text: root.oskEnabled ? root.layout : "disabled"
            color: root.bar ? Qt.darker(root.bar.foreground, 1.4) : Qt.darker(Color.foreground, 1.4)
            font.family: root.bar ? root.bar.fontFamily : Style.font.family
            font.pixelSize: Style.font.caption
            font.letterSpacing: 1.2
            textFormat: Text.PlainText
          }
        }
      }

      Toggle {
        width: parent.width
        label: "Enabled"
        description: "Bar toggle, swipe-up gesture and SUPER+SHIFT+K summon the keyboard"
        checked: root.oskEnabled
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        onClicked: root.setOskEnabled(!root.oskEnabled)
      }

      Button {
        width: parent.width
        text: root.oskEnabled ? "Hide keyboard" : "Show keyboard"
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        enabled: root.oskEnabled
        onClicked: root.toggleOsk()
      }

      PanelSeparator {
        width: parent.width
        foreground: root.bar ? root.bar.foreground : Color.foreground
      }

      // ---------- layout ----------
      PanelSectionHeader {
        width: parent.width
        text: "Layout"
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
      }

      Text {
        width: parent.width
        text: "Any layout installed under /usr/share/X11/xkb; variants like us(intl) work via the keyboard plugin's IPC."
        wrapMode: Text.WordWrap
        color: root.bar ? Qt.darker(root.bar.foreground, 1.4) : Qt.darker(Color.foreground, 1.4)
        font.family: root.bar ? root.bar.fontFamily : Style.font.family
        font.pixelSize: Style.font.caption
      }

      Dropdown {
        width: parent.width
        label: "Keyboard layout"
        value: root.layout
        options: root.layouts
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        onChanged: function(v) {
          root.layout = v
          root.callOsk("setLayout", v)
        }
      }

      PanelSeparator {
        width: parent.width
        foreground: root.bar ? root.bar.foreground : Color.foreground
      }

      // ---------- key repeat ----------
      PanelSectionHeader {
        width: parent.width
        text: "Key repeat (hold any key)"
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
      }

      Toggle {
        width: parent.width
        label: "Enable repeat"
        description: "Holding a key repeats it (first after the delay, then at the interval)"
        checked: root.repeatEnabled
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        onClicked: {
          root.repeatEnabled = !root.repeatEnabled
          root.callOsk("setRepeatEnabled", root.repeatEnabled ? "on" : "off")
        }
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Delay · " + root.repeatDelay + " ms"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.repeatDelay
          minimum: 100
          maximum: 1000
          step: 50
          integer: true
          onMoved: function(v) { root.repeatDelay = Math.round(v) }
          onReleased: function(v) { root.callOsk("setRepeat", root.repeatDelay + " " + root.repeatInterval) }
        }
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Interval · " + root.repeatInterval + " ms"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.repeatInterval
          minimum: 15
          maximum: 250
          step: 5
          integer: true
          onMoved: function(v) { root.repeatInterval = Math.round(v) }
          onReleased: function(v) { root.callOsk("setRepeat", root.repeatDelay + " " + root.repeatInterval) }
        }
      }
    }
  }

  FileView {
    id: cfgFile
    path: (Quickshell.env("XDG_CONFIG_HOME") || (Quickshell.env("HOME") + "/.config")) + "/omarchy/osk.json"
    watchChanges: true
    atomicWrites: true
    printErrors: false
    blockLoading: true
    onLoaded: root.applyConfig(text())
    onLoadFailed: root.applyConfig("")
    onFileChanged: reload()
  }

  Process {
    id: layoutsProc
    command: ["localectl", "list-x11-keymap-layouts"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.setLayouts(text)
    }
  }
}
