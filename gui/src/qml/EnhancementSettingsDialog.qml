import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

import org.streetpea.chiaking

import "controls" as C

// Video enhancement settings that apply while streaming: opened from the
// stream menu (Ctrl+O) like the Display and Placebo panels. Every change
// takes effect on the next rendered frame.
DialogView {
    id: dialog
    property alias gridHeight: enhancementGrid.height
    title: qsTr("Enhancement")
    header: qsTr("* Applies immediately, defaults in () to the right")
    buttonVisible: false

    Item {
        GridLayout {
            id: enhancementGrid
            anchors {
                top: parent.top
                horizontalCenter: parent.horizontalCenter
                topMargin: 20
            }
            columns: 3
            rowSpacing: 10
            columnSpacing: 20

            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Active:")
            }

            Label {
                Layout.preferredWidth: 600
                text: Chiaki.window.videoEnhancementStatus.length ? Chiaki.window.videoEnhancementStatus : qsTr("None")
                color: Material.accent
                font.bold: true
                elide: Text.ElideRight
            }

            Label {
                text: Chiaki.session ? qsTr("%1 / %2 fps").arg(Chiaki.window.streamFps.toFixed(0)).arg(Chiaki.window.displayFps.toFixed(0)) : ""
                color: "#ffe082"
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.rtxVsrSupported
                text: qsTr("RTX Video Super Resolution:")
            }

            C.ComboBox {
                id: rtxVsrCombo
                firstInFocusChain: true
                Layout.preferredWidth: 600
                visible: Chiaki.settings.rtxVsrSupported
                // NGX VSR.QualityLevel for each entry
                readonly property var modes: [0, 1, 2, 3, 4, 8, 9, 10, 11, 21, 23]
                model: [qsTr("Off"),
                    qsTr("Upscale Low"), qsTr("Upscale Medium"), qsTr("Upscale High"), qsTr("Upscale Ultra"),
                    qsTr("Artifact Reduction Low"), qsTr("Artifact Reduction Medium"),
                    qsTr("Artifact Reduction High"), qsTr("Artifact Reduction Ultra"),
                    qsTr("Streaming Medium (RTX 30+)"), qsTr("Streaming Ultra (RTX 30+)")]
                currentIndex: Math.max(0, modes.indexOf(Chiaki.settings.rtxVsrQuality))
                // Artifact Reduction / Streaming need nvngx_vsr.dll 1.8.2+.
                function modeAvailable(index) {
                    return modes[index] <= 4 || Chiaki.settings.rtxVsrExtendedModes;
                }
                onActivated: index => {
                    if (modeAvailable(index))
                        Chiaki.settings.rtxVsrQuality = modes[index];
                    else
                        currentIndex = Qt.binding(() => Math.max(0, modes.indexOf(Chiaki.settings.rtxVsrQuality)));
                }
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: ListView.view ? ListView.view.width : implicitWidth
                    enabled: rtxVsrCombo.modeAvailable(index)
                    text: enabled ? modelData : qsTr("%1 (needs nvngx_vsr.dll 1.8.2+)").arg(modelData)
                    highlighted: rtxVsrCombo.highlightedIndex === index
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Installed nvngx_vsr.dll: %1").arg(Chiaki.settings.rtxVsrDllVersion.length ? Chiaki.settings.rtxVsrDllVersion : qsTr("not found"))
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.rtxVsrSupported
                text: qsTr("(Off)")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenSupported
                text: qsTr("Frame Generation:")
            }

            C.ComboBox {
                firstInFocusChain: !Chiaki.settings.rtxVsrSupported
                Layout.preferredWidth: 600
                visible: Chiaki.settings.frameGenSupported
                enabled: Chiaki.settings.directFrameMapping
                model: [qsTr("Off"),
                    Chiaki.settings.frameGenDlssSupported ? qsTr("NVIDIA DLSS Frame Generation") : qsTr("NVIDIA DLSS Frame Generation (not in this build)"),
                    Chiaki.settings.frameGenFsrSupported ? qsTr("AMD FSR 3.1 Frame Generation") : qsTr("AMD FSR 3.1 Frame Generation (not in this build)"),
                    qsTr("Flow Frame Generation (any GPU)")]
                currentIndex: Chiaki.settings.frameGenBackend
                onActivated: index => Chiaki.settings.frameGenBackend = index
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Needs a display running at least twice the stream frame rate. HDR streams are shown without frame generation.")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenSupported
                text: Chiaki.settings.directFrameMapping ? qsTr("(Off)") : qsTr("(needs Direct Mapping)")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenFlowSupported
                text: qsTr("Flow Quality:")
            }

            C.ComboBox {
                Layout.preferredWidth: 600
                visible: Chiaki.settings.frameGenFlowSupported
                enabled: Chiaki.settings.frameGenBackend === 3
                model: [qsTr("Performance (1/4 resolution motion analysis)"), qsTr("Quality (1/2 resolution motion analysis)")]
                currentIndex: Chiaki.settings.frameGenFlowHighQuality ? 1 : 0
                onActivated: index => Chiaki.settings.frameGenFlowHighQuality = index === 1
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenFlowSupported
                text: qsTr("(Performance)")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenSupported
                text: qsTr("Frame Generation Multiplier:")
            }

            C.ComboBox {
                Layout.preferredWidth: 600
                visible: Chiaki.settings.frameGenSupported
                enabled: Chiaki.settings.frameGenBackend === 1 || Chiaki.settings.frameGenBackend === 3
                model: [qsTr("2x"), qsTr("3x (DLSS on RTX 50 series, Flow)"), qsTr("4x (DLSS on RTX 50 series, Flow)")]
                currentIndex: (Chiaki.settings.frameGenBackend === 1 || Chiaki.settings.frameGenBackend === 3) ? Chiaki.settings.frameGenMultiplier - 2 : 0
                onActivated: index => Chiaki.settings.frameGenMultiplier = index + 2
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Output frames per stream frame. FSR always uses 2x; the display refresh rate also limits it.")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.frameGenSupported
                text: qsTr("(2x)")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Sharpening (AMD RCAS):")
            }

            C.Slider {
                Layout.preferredWidth: 400
                from: 0
                to: 100
                stepSize: 5
                lastInFocusChain: !Chiaki.settings.ambientBorderSupported
                value: Chiaki.settings.rcasStrength
                onMoved: Chiaki.settings.rcasStrength = value
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Contrast adaptive sharpening of the final picture. 0 = off. Lower it if edges look harsh or compression noise shows.")
                Label {
                    anchors {
                        left: parent.right
                        verticalCenter: parent.verticalCenter
                        leftMargin: 10
                    }
                    text: parent.value > 0 ? qsTr("%1%").arg(parent.value.toFixed(0)) : qsTr("Off")
                }
            }

            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("(Off)")
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.ambientBorderSupported
                text: qsTr("Border Fill:")
            }

            C.ComboBox {
                Layout.preferredWidth: 600
                visible: Chiaki.settings.ambientBorderSupported
                lastInFocusChain: true
                model: [qsTr("Black"), qsTr("Ambient (blurred picture)")]
                currentIndex: Chiaki.settings.ambientBorder ? 1 : 0
                onActivated: index => Chiaki.settings.ambientBorder = index === 1
            }

            Label {
                Layout.alignment: Qt.AlignRight
                visible: Chiaki.settings.ambientBorderSupported
                text: qsTr("(Black)")
            }
        }
    }
}
