// AI Tracking.
//   * AI Track   — Human tracking (gimbal follows a person; LED turns blue).
//   * Face Track — AiWorkModePortraitTrack, documented in the SDK header as
//     "Portrait tracking for tiny3" — a genuine Tiny3-specific mode, framed
//     tighter on the face. Shares the device's single ai_mode field with AI
//     Track, so the two are mutually exclusive (picking one turns the other off).
//   * Face focus — cameraSetFaceFocusR, autofocus-only. Independent of both
//     tracking modes: it never moves the gimbal or turns the LED blue.
//   * Gesture control — independent, SDK rc logged honestly.
//
// NOTE: an earlier build exposed a "Framing" (Normal/Upper body/Close-up)
// control that reused cameraSetAiModeU's second parameter. That parameter's
// AiSubModeType meaning is documented in the SDK header as "for tiny2" only —
// on Tiny3 it produced inconsistent zoom behavior instead of real framing, so
// it was removed and replaced with the Face Track mode above, which Tiny3
// genuinely documents.
// Tracking zone — limit-area tracking via the tail2+ control-parameter API
// (hardware-verified on Tiny 3): edit + Apply the max zone, then toggle zone
// tracking. All values shown are device readback.
// Motion speed — GimCtrlSpeedMode control parameter (verified on Tiny 3).
// Advanced sensitivity control remains capability-gated OFF
// (no verified Tiny 3 setters).
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Obsbot

RowLayout {
    id: root
    spacing: Theme.s4

    // One editable zone edge, drawn as a box centered on a side of the zone
    // square: − [value°] + (5° per click, clamped). Opaque so it masks the
    // frame line. Right-click fills it from the camera's current position.
    component ZoneEdge: Rectangle {
        id: ze
        property string edge: ""   // panMin | panMax | pitchMin | pitchMax
        property real value: 0
        property real lo: -100
        property real hi: 100
        signal edited(real v)
        width: st.implicitWidth + 8
        height: st.implicitHeight + 8
        radius: Theme.rControl
        color: Theme.bg
        Stepper {
            id: st
            anchors.centerIn: parent
            valueText: ze.value.toFixed(1) + "°"
            onDecrement: ze.edited(Math.max(ze.lo, ze.value - 5))
            onIncrement: ze.edited(Math.min(ze.hi, ze.value + 5))
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            enabled: ze.enabled
            onTapped: cam.captureZoneEdge(ze.edge)
        }
    }

    // Scrolls when the window is too short for every panel (same pattern as
    // ControlPage's controls column).
    Flickable {
        id: trackFlick
        Layout.fillWidth: true
        Layout.maximumWidth: 570
        Layout.fillHeight: true
        contentHeight: trackCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            id: trackScroll
            policy: ScrollBar.AsNeeded
            // Auto-hide: only visible while actively scrolling or hovered.
            opacity: (trackFlick.moving || trackScroll.pressed || trackScroll.hovered) ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 250 } }
        }
    ColumnLayout {
        id: trackCol
        // leave room on the right so panels never sit under the scrollbar
        width: trackFlick.width - 10
        spacing: Theme.s3

        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: aiCol.implicitHeight + 28
            ColumnLayout {
                id: aiCol
                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.s3

                RowLayout {
                    Layout.fillWidth: true
                    SectionLabel { text: "AI & Gesture" }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 9; height: 9; radius: 4.5
                        visible: cam.aiPending
                        color: Theme.busy
                        SequentialAnimation on opacity {
                            running: cam.aiPending; loops: Animation.Infinite
                            NumberAnimation { from: 0.3; to: 1.0; duration: 450 }
                            NumberAnimation { from: 1.0; to: 0.3; duration: 450 }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ToggleChip {
                        Layout.fillWidth: true
                        text: "AI Track"; tone: Theme.busy
                        enabled: cam.connected && cam.capAi
                        busy: cam.aiPending
                        checked: cam.aiTracking
                        onToggled: (c) => cam.setAiTracking(c)
                    }
                    ToggleChip {
                        Layout.fillWidth: true
                        text: "Gesture control"; tone: Theme.live
                        enabled: cam.connected && cam.capGesture
                        checked: cam.gesture
                        onToggled: (c) => cam.setGesture(c)
                    }
                }
                // Who AI Track follows: Solo (AiWorkModeHuman) or Group
                // (AiWorkModeGroup). Switching while tracking re-sends live.
                Segmented {
                    Layout.fillWidth: true
                    enabled: cam.connected && cam.capAi
                    options: ["Solo", "Group"]
                    currentIndex: cam.trackTarget
                    onActivated: (i) => cam.trackTarget = i
                }
                // Auto zoom framing (aiSetTargetZoomTypeR) — only acts while AI
                // Track is on in Solo (the device ignores it in Group, which frames
                // everyone itself); the selection follows the device's ai_sub_mode.
                SectionLabel { text: cam.trackTarget === 1 ? "Auto zoom (Solo only)" : "Auto zoom" }
                Segmented {
                    Layout.fillWidth: true
                    enabled: cam.connected && cam.aiTracking && cam.trackTarget === 0
                    options: ["Off", "Full", "Upper", "Close", "No head", "Lower"]
                    currentIndex: cam.autoZoom
                    onActivated: (i) => cam.autoZoom = i
                }
                // Face autofocus — independent of tracking, and subtle. Kept
                // separate + honestly labeled so it isn't mistaken for a tracking
                // control that "does nothing".
                ToggleChip {
                    Layout.fillWidth: true
                    text: "Face autofocus"; tone: Theme.live
                    enabled: cam.connected && cam.capFace
                    checked: cam.faceFocus
                    onToggled: (c) => cam.setFaceFocus(c)
                }
                Text {
                    text: "AI Track is the tracking mode: the camera follows a person and keeps their face framed (LED turns blue; manual PTZ is blocked while it's on). "
                        + "Face autofocus just biases focus toward faces — it's independent of tracking and does NOT move the gimbal or the LED, so its effect is subtle. "
                        + "Gesture control: open palm beside your face starts/stops tracking; the L-shape gesture zooms. "
                        + "Detection can be unreliable while a control app is attached (under investigation, issue #11)."
                    color: Theme.dimmer
                    font.family: Theme.sans
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                // Experimental, OPT-IN: while gesture is on, slow the status
                // refresh to one per ~15 s (frequent polling suppresses the
                // camera's gesture recognizer). Off = normal behavior, so both
                // ways can be A/B tested across firmware updates.
                ToggleChip {
                    Layout.fillWidth: true
                    text: "Low-traffic mode while gestures on (experimental)"
                    tone: Theme.degraded
                    enabled: cam.connected
                    checked: cam.gestureLowTraffic
                    onToggled: (c) => cam.gestureLowTraffic = c
                }
                // Diagnostic: pauses ALL periodic SDK traffic for 60 s so we can
                // tell whether our status polling or the mere app session is what
                // suppresses the camera's gesture recognizer.
                ActionButton {
                    text: "Gesture test: pause app traffic 60 s"
                    variant: "secondary"
                    enabled: cam.connected
                    onClicked: cam.gestureQuietTest()
                }
                KeyValue { key: "tracking mode"; value: cam.connected ? cam.aiModeName : "—"; unknown: !cam.connected }
                // Tracking speed (GimCtrlSpeedMode). The selection is the device
                // readback; none is highlighted if the camera is in another mode.
                SectionLabel { text: "Motion speed" }
                Segmented {
                    Layout.fillWidth: true
                    enabled: cam.connected
                    options: ["Slow", "Std", "Fast"]
                    currentIndex: cam.trackSpeed
                    onActivated: (i) => cam.trackSpeed = i
                }
            }
        }

        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: zoneCol.implicitHeight + 28
            ColumnLayout {
                id: zoneCol
                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.s3
                enabled: cam.connected && cam.zoneValid

                // Draft limits: edited locally, sent to the device only on
                // Apply, and re-synced from the device readback after it.
                property real panMin: 0
                property real panMax: 0
                property real pitchMin: 0
                property real pitchMax: 0
                function syncFromDevice() {
                    panMin = cam.zonePanMin
                    panMax = cam.zonePanMax
                    pitchMin = cam.zonePitchMin
                    pitchMax = cam.zonePitchMax
                }
                Component.onCompleted: syncFromDevice()
                Connections {
                    target: cam
                    function onZoneChanged() { zoneCol.syncFromDevice() }
                    function onZoneEdgeCaptured(edge, value) {
                        const pan = edge.startsWith("pan")
                        const lo = pan ? cam.zoneDefPanMin : cam.zoneDefPitchMin
                        const hi = pan ? cam.zoneDefPanMax : cam.zoneDefPitchMax
                        zoneCol[edge] = Math.max(lo, Math.min(hi, value))
                    }
                }

                SectionLabel { text: "Tracking zone" }
                // The zone as a square, laid out like the official Windows app
                // (verified by reading back a zone set there): left side = pan
                // max, right side = pan min, top = tilt max, bottom = tilt min
                // (tilt shown up-positive; the worker converts device space).
                Item {
                    Layout.fillWidth: true
                    implicitHeight: 230
                    Rectangle {
                        id: zoneFrame
                        anchors.fill: parent
                        anchors.leftMargin: edgeLeft.width / 2
                        anchors.rightMargin: edgeRight.width / 2
                        anchors.topMargin: edgeTop.height / 2
                        anchors.bottomMargin: edgeBottom.height / 2
                        radius: 4
                        color: Qt.rgba(Theme.busy.r, Theme.busy.g, Theme.busy.b, cam.zoneTracking ? 0.10 : 0.04)
                        border.width: 1
                        border.color: Qt.rgba(Theme.busy.r, Theme.busy.g, Theme.busy.b, cam.zoneTracking ? 0.8 : 0.35)
                        Text {
                            anchors.centerIn: parent
                            text: "+"
                            color: Theme.dimmer
                            font.family: Theme.mono
                            font.pixelSize: 16
                        }
                    }
                    ZoneEdge {
                        id: edgeTop
                        anchors.horizontalCenter: zoneFrame.horizontalCenter
                        anchors.verticalCenter: zoneFrame.top
                        edge: "pitchMax"; value: zoneCol.pitchMax
                        lo: zoneCol.pitchMin + 5; hi: cam.zoneDefPitchMax
                        onEdited: (v) => zoneCol.pitchMax = v
                    }
                    ZoneEdge {
                        id: edgeBottom
                        anchors.horizontalCenter: zoneFrame.horizontalCenter
                        anchors.verticalCenter: zoneFrame.bottom
                        edge: "pitchMin"; value: zoneCol.pitchMin
                        lo: cam.zoneDefPitchMin; hi: zoneCol.pitchMax - 5
                        onEdited: (v) => zoneCol.pitchMin = v
                    }
                    ZoneEdge {
                        id: edgeLeft
                        anchors.horizontalCenter: zoneFrame.left
                        anchors.verticalCenter: zoneFrame.verticalCenter
                        edge: "panMax"; value: zoneCol.panMax
                        lo: zoneCol.panMin + 5; hi: cam.zoneDefPanMax
                        onEdited: (v) => zoneCol.panMax = v
                    }
                    ZoneEdge {
                        id: edgeRight
                        anchors.horizontalCenter: zoneFrame.right
                        anchors.verticalCenter: zoneFrame.verticalCenter
                        edge: "panMin"; value: zoneCol.panMin
                        lo: cam.zoneDefPanMin; hi: zoneCol.panMax - 5
                        onEdited: (v) => zoneCol.panMin = v
                    }
                }
                RowLayout {
                    spacing: 8
                    ActionButton {
                        text: "Apply zone"
                        action: "zone limits"
                        enabled: zoneCol.enabled
                        onClicked: cam.setZoneLimits(zoneCol.panMin, zoneCol.panMax,
                                                     zoneCol.pitchMin, zoneCol.pitchMax)
                    }
                    ActionButton {
                        text: "Reset to default"
                        variant: "secondary"
                        enabled: zoneCol.enabled
                        onClicked: cam.resetZoneLimits()
                    }
                }
                ToggleChip {
                    Layout.fillWidth: true
                    text: "Zone tracking"; tone: Theme.busy
                    enabled: zoneCol.enabled
                    checked: cam.zoneTracking
                    onToggled: (c) => cam.setZoneTracking(c)
                }
                Text {
                    text: cam.connected && !cam.zoneValid
                          ? "Zone parameters are not readable on this device."
                          : "Set the max zone and Apply it, then turn zone tracking on. While AI Track is on, "
                            + "the camera only follows you inside these pan/tilt limits (degrees from center). "
                            + "Left/right boxes = pan, top/bottom = tilt. "
                            + "Right-click a box to take the camera's current position. "
                            + "The zone is stored in the camera."
                    color: cam.connected && !cam.zoneValid ? Theme.degraded : Theme.dimmer
                    font.family: Theme.sans
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            radius: Theme.rControl
            color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(Theme.degraded.r, Theme.degraded.g, Theme.degraded.b, 0.4)
            implicitHeight: tbanner.implicitHeight + 20
            Text {
                id: tbanner
                anchors.fill: parent; anchors.margins: 10
                text: "Sensitivity is disabled — " + cam.capUnverifiedReason
                color: Theme.degraded
                font.family: Theme.mono; font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        GlassPanel {
            Layout.fillWidth: true
            implicitHeight: advCol.implicitHeight + 28
            ColumnLayout {
                id: advCol
                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.s3
                enabled: cam.capTrackingAdvanced   // false
                RowLayout {
                    spacing: 10
                    Text { text: "Sensitivity"; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12; Layout.preferredWidth: 96 }
                    Slider { Layout.fillWidth: true; from: 0; to: 100; value: 50; enabled: false }
                }
            }
        }
    }
    }

    GlassPanel {
        Layout.preferredWidth: 460
        Layout.maximumWidth: 480
        Layout.preferredHeight: 310
        Layout.alignment: Qt.AlignTop
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8
            SectionLabel { text: "Reference" }
            Viewfinder { Layout.fillWidth: true; Layout.fillHeight: true }
        }
    }
}
