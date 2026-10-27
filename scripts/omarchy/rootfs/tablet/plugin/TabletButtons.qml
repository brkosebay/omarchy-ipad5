import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons

// linux-a9: a row of touch buttons on the bar for the iPad (no mouse, maybe
// no keyboard). Each runs an ipad-tablet action, the same ones the touch
// gestures run (docs/omarchy/tablet-usage.md). Right or middle click gives
// the second action where there is one. The rotation-lock button shows the
// lock (ipad-rotate lock), read from the same state file ipad-rotate writes;
// it only appears while ipad-autorotate runs (there is nothing to lock before
// the iPad has an accelerometer). The auto-brightness button likewise only
// appears while ipad-als-brightness runs (the kernel has the ambient light
// sensor) and is lit while auto-brightness is on; it is off by default.
BarWidget {
  id: root
  moduleName: "linuxa9.tablet"

  readonly property string stateFile:
    (Quickshell.env("XDG_STATE_HOME") || (Quickshell.env("HOME") + "/.local/state")) + "/linux-a9/display"
  property bool locked: false
  property bool autorotate: false
  property bool alsDaemon: false
  property bool autoBright: false
  // The 768-wide portrait desktop has no room for every button next to the
  // clock: the ones marked here (they all have a gesture or a key) step aside.
  readonly property bool portrait: Screen.width < Screen.height

  FileView {
    id: displayState
    path: root.stateFile
    watchChanges: true
    printErrors: false
    onLoaded: root.locked = /^\s*rotation_lock\s*=\s*on\s*$/m.test(text())
    onLoadFailed: root.locked = false
    onFileChanged: reload()
  }

  // ipad-autorotate keeps this file while it runs. The file's creation isn't
  // watched, so look again now and then.
  FileView {
    id: autorotateStatus
    path: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/ipad-autorotate.json"
    printErrors: false
    onLoaded: root.autorotate = true
    onLoadFailed: root.autorotate = false
  }
  Timer {
    interval: 4000
    running: true
    repeat: true
    onTriggered: autorotateStatus.reload()
  }

  // ipad-als-brightness keeps this file while it runs, with its switch in it.
  FileView {
    id: alsStatus
    path: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/ipad-als-brightness.json"
    printErrors: false
    onLoaded: {
      root.alsDaemon = true
      root.autoBright = /"enabled":\s*true/.test(text())
    }
    onLoadFailed: {
      root.alsDaemon = false
      root.autoBright = false
    }
  }
  Timer {
    interval: 2000
    running: true
    repeat: true
    onTriggered: alsStatus.reload()
  }
  // A press: the daemon sees the switch and rewrites its file within a second.
  Timer {
    id: alsAfterPress
    interval: 1300
    onTriggered: alsStatus.reload()
  }

  readonly property var buttons: [
    { icon: 0xF003B, tip: "Apps (swipe up from the bottom edge)", cmd: "ipad-tablet apps", alt: "ipad-tablet menu" },
    { icon: 0xF030C, tip: "On-screen keyboard (swipe down from the top edge)", cmd: "ipad-tablet keyboard" },
    { icon: 0xF056E, portraitHides: true, tip: "Layout: tiling, tablet, floating", cmd: "ipad-tablet layout next" },
    { icon: 0xF0465, tip: "Rotate screen", cmd: "ipad-tablet rotate" },
    { icon: 0xF0156, tip: "Close window (four fingers down)", cmd: "ipad-tablet close" }
    // Float/fullscreen are gestures; auto-brightness and rotation lock live in
    // the iPad Screen menu. Nine buttons overflowed the vertical bar.
  ]

  function run(command) {
    if (command) Quickshell.execDetached(["bash", "-lc", command])
  }

  implicitWidth: grid.implicitWidth
  implicitHeight: grid.implicitHeight

  Grid {
    id: grid
    anchors.centerIn: parent
    rows: root.vertical ? root.buttons.length : 1
    columns: root.vertical ? 1 : root.buttons.length
    spacing: 0

    Repeater {
      model: root.buttons

      BarIconButton {
        required property var modelData
        visible: !(root.portrait && modelData.portraitHides) && !(modelData.lock && !root.autorotate) &&
                 !(modelData.als && !root.alsDaemon)
        bar: root.bar
        // Wider than the shell's 8.5: a fingertip, not a pointer.
        horizontalMargin: 13
        text: String.fromCodePoint(modelData.lock && root.locked ? modelData.lockedIcon
                                   : modelData.als && root.autoBright ? modelData.litIcon : modelData.icon)
        active: (!!modelData.lock && root.locked) || (!!modelData.als && root.autoBright)
        tooltipText: modelData.lock ? modelData.tip + (root.locked ? ": locked" : ": unlocked")
                     : modelData.als ? modelData.tip + (root.autoBright ? ": on" : ": off") : modelData.tip
        onPressed: function(b) {
          root.run(b === Qt.LeftButton || !modelData.alt ? modelData.cmd : modelData.alt)
          if (modelData.als) alsAfterPress.restart()
        }
      }
    }
  }
}
