import QtQuick
import QtQuick.Controls
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
  property int flingDecay: 320
  property int flingCap: 5500
  property int dragSlop: 12
  property int longPress: 450
  property int scrollGain: 100
  property bool scrollAxisPx: false
  property bool touchSwallow: true
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

  function shellQuote(s) {
    return "'" + String(s).replace(/'/g, "'\\''") + "'"
  }

  // Settings changes ride the OSK plugin's own IPC target.
  function callOsk(method, arg) {
    if (!root.bar)
      return
    const parts = String(arg).trim().split(/\s+/).map(root.shellQuote).join(" ")
    root.bar.run("omarchy-shell ekollof.osk " + method + " " + parts)
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
    if (cfg.flingDecay)
      root.flingDecay = parseInt(cfg.flingDecay, 10) || 320
    if (cfg.flingCap)
      root.flingCap = parseInt(cfg.flingCap, 10) || 5500
    if (cfg.dragSlop)
      root.dragSlop = parseInt(cfg.dragSlop, 10) || 12
    if (cfg.longPress !== undefined)
      root.longPress = parseInt(cfg.longPress, 10) || 0
    if (cfg.scrollGain)
      root.scrollGain = parseInt(cfg.scrollGain, 10) || 100
    if (cfg.scrollAxisPx !== undefined)
      root.scrollAxisPx = !!cfg.scrollAxisPx
    if (cfg.touchSwallow !== undefined)
      root.touchSwallow = !!cfg.touchSwallow
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

    Item {
      anchors.fill: parent

    Flickable {
      id: panelFlick
      anchors.fill: parent
      readonly property bool overflow: contentHeight > height + 1
      contentWidth: width
      contentHeight: column.implicitHeight
      clip: true
      boundsBehavior: Flickable.StopAtBounds
      flickableDirection: Flickable.VerticalFlick
      // One-finger drags land on sliders/toggles, so don't rely on
      // Flickable stealing the press. Wheel/two-finger and the scrollbar do.
      interactive: false
      ScrollBar.vertical: ScrollBar {
        policy: panelFlick.overflow ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        implicitWidth: Style.space(16)
        interactive: true
      }

      Column {
        id: column
        width: panelFlick.width - (panelFlick.overflow ? Style.space(20) : 0)
        spacing: Style.space(12)

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
            Component.onCompleted: console.log("[ekollof.osk-applet] loaded rev6")
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

      Toggle {
        width: parent.width
        label: "Virtual pointing device"
        description: root.touchSwallow
                     ? "On: touch input drives the virtual pointer (tap, drag, scroll, pinch)"
                     : "Off: native touchscreen support — apps receive raw touch, gestures are off"
        checked: root.touchSwallow
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        onClicked: {
          root.touchSwallow = !root.touchSwallow
          root.callOsk("setTouchSwallow", root.touchSwallow ? "on" : "off")
        }
      }

      Button {
        width: parent.width
        text: "Toggle keyboard"
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

      // Pointer/scroll knobs only apply while the compositor swallows
      // touch; hide them in native-touchscreen mode so the panel is shorter.
      Column {
        width: parent.width
        spacing: Style.space(12)
        visible: root.touchSwallow

      PanelSectionHeader {
        width: parent.width
        text: "Pointer (touch → mouse)"
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Drag slop · " + root.dragSlop + " px"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.dragSlop
          minimum: 4
          maximum: 40
          step: 1
          integer: true
          onMoved: function(v) { root.dragSlop = Math.round(v) }
          onReleased: function(v) { root.callOsk("setPointer", root.dragSlop + " " + root.longPress) }
        }
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: root.longPress === 0 ? "Long-press right click · off" : "Long-press right click · " + root.longPress + " ms"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.longPress
          minimum: 0
          maximum: 1000
          step: 50
          integer: true
          onMoved: function(v) { root.longPress = Math.round(v) }
          onReleased: function(v) { root.callOsk("setPointer", root.dragSlop + " " + root.longPress) }
        }
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Scroll speed · " + root.scrollGain + "%"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.scrollGain
          minimum: 50
          maximum: 200
          step: 10
          integer: true
          onMoved: function(v) { root.scrollGain = Math.round(v) }
          onReleased: function(v) { root.callOsk("setScroll", root.scrollGain + " " + (root.scrollAxisPx ? "1" : "0")) }
        }
      }

      Toggle {
        width: parent.width
        label: "Pixel axis value"
        description: root.scrollAxisPx
                     ? "On: force pixel axis for every app (terminals already get this automatically)"
                     : "Off: auto — terminals get pixels, Chromium-style clients get the ×12-safe axis"
        checked: root.scrollAxisPx
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
        onClicked: {
          root.scrollAxisPx = !root.scrollAxisPx
          root.callOsk("setScrollAxis", root.scrollAxisPx ? "on" : "off")
        }
      }

      PanelSeparator {
        width: parent.width
        foreground: root.bar ? root.bar.foreground : Color.foreground
      }

      // ---------- fling ----------
      PanelSectionHeader {
        width: parent.width
        text: "Scroll fling (momentum after lifting the fingers)"
        foreground: root.bar ? root.bar.foreground : Color.foreground
        fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Glide · " + root.flingDecay + " ms"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.flingDecay
          minimum: 100
          maximum: 800
          step: 20
          integer: true
          onMoved: function(v) { root.flingDecay = Math.round(v) }
          onReleased: function(v) { root.callOsk("setFling", root.flingDecay + " " + root.flingCap) }
        }
      }

      Column {
        width: parent.width
        spacing: Style.space(4)

        Text {
          text: "Speed cap · " + root.flingCap + " px/s"
          color: root.bar ? root.bar.foreground : Color.foreground
          font.family: root.bar ? root.bar.fontFamily : Style.font.family
          font.pixelSize: Style.font.caption
        }

        PanelSlider {
          width: parent.width
          bar: root.bar
          value: root.flingCap
          minimum: 1000
          maximum: 8000
          step: 250
          integer: true
          onMoved: function(v) { root.flingCap = Math.round(v) }
          onReleased: function(v) { root.callOsk("setFling", root.flingDecay + " " + root.flingCap) }
        }
      }
      } // virtual-pointer settings
      } // column
    } // flickable

    // Two-finger scroll is a pointer axis (wheel). PanelSlider eats wheel
    // to nudge its value, so capture it above the controls. NoButton keeps
    // taps and slider drags going through to the children below.
    MouseArea {
      anchors.fill: parent
      anchors.rightMargin: panelFlick.overflow ? Style.space(18) : 0
      z: 10
      acceptedButtons: Qt.NoButton
      onWheel: function(wheel) {
        // hypr-osk two-finger scroll is high-res v120 with 1 unit ≈ 1 px
        // (SOURCE_WHEEL, so Qt leaves pixelDelta empty). Notch wheels send
        // multiples of 120; those become a ~48 px step. Dividing by 8 made
        // virtual-pointer scrolling ~8× too slow.
        const a = wheel.angleDelta.y
        let dy = wheel.pixelDelta.y
        // Finger-source axis: Qt pixelDelta is the tiny legacy value (px/12);
        // v120 lands in angleDelta as 1:1 pixels. Prefer that.
        if (dy === 0 || Math.abs(a) > Math.abs(dy) * 2)
          dy = (Math.abs(a) >= 120 && a % 120 === 0) ? (a / 120) * 48 : a
        const maxY = Math.max(0, panelFlick.contentHeight - panelFlick.height)
        panelFlick.contentY = Math.max(0, Math.min(maxY, panelFlick.contentY - dy))
        wheel.accepted = true
      }
    }
    } // viewport
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
