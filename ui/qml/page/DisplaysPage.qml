pragma ValueTypeBehavior: Assertable
import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Templates as T
import Qcm.Material as MD
import waywallen.ui as W

MD.Page {
    id: root

    title: 'Displays'
    showHeader: true
    showBackground: false
    readonly property real displayGapPx: 80

    property var selectedId: null

    // FillMode/Align enum values mirror proto::FillMode / proto::Align
    // (control.proto). Keep the *_VALUES arrays in lockstep with the
    // enum order; *_LABELS is what the UI shows.
    readonly property var kFillModeValues: [
        1, // STRETCHED
        2, // PRESERVE_ASPECT_FIT
        3, // PRESERVE_ASPECT_CROP
        7, // CENTERED
        4, // TILED
        5, // TILED_ONLY_HORIZONTAL
        6  // TILED_ONLY_VERTICAL
    ]
    readonly property var kFillModeLabels: [
        "Stretch",
        "Fit (preserve aspect)",
        "Crop (preserve aspect)",
        "Center (1:1)",
        "Tile",
        "Tile horizontally",
        "Tile vertically"
    ]
    function fillmodeIndex(value) {
        const i = root.kFillModeValues.indexOf(value);
        return i < 0 ? 0 : i;
    }

    // 3×3 align grid; index = row * 3 + col, values match proto::Align.
    readonly property var kAlignValues: [
        1, 2, 3, // top-left, top, top-right
        4, 5, 6, // left, center, right
        7, 8, 9  // bottom-left, bottom, bottom-right
    ]
    readonly property var kAlignTooltips: [
        "Top-left", "Top", "Top-right",
        "Left", "Center", "Right",
        "Bottom-left", "Bottom", "Bottom-right"
    ]

    W.DisplayLayoutSetQuery { id: layoutSetQuery }

    function layoutRects() {
        const out = [];
        let x = 0;
        for (const d of W.App.displayManager.displays || []) {
            out.push({
                x: x,
                y: 0,
                w: d.width,
                h: d.height,
                d: d
            });
            x += d.width + root.displayGapPx;
        }
        return out;
    }

    readonly property var rects: layoutRects()

    readonly property real boundsW: {
        let max = 0;
        for (const r of rects)
            max = Math.max(max, r.x + r.w);
        return max || 1;
    }
    readonly property real boundsH: {
        let max = 0;
        for (const r of rects)
            max = Math.max(max, r.y + r.h);
        return max || 1;
    }

    function selectedDisplay() {
        if (root.selectedId === null)
            return null;
        for (const d of W.App.displayManager.displays || []) {
            if (d.id === root.selectedId)
                return d;
        }
        return null;
    }

    readonly property var selected: selectedDisplay()

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 16

        MD.Pane {
            id: displaysPane
            Layout.fillWidth: true
            Layout.fillHeight: true
            leftPadding: 16
            rightPadding: 16
            radius: 16
            backgroundColor: MD.MProp.color.surface

            contentItem: Item {
                id: canvas
                implicitHeight: 48

                readonly property real padding: 24
                readonly property real viewScale: {
                    const availW = Math.max(1, width - padding * 2);
                    const availH = Math.max(1, height - padding * 2);
                    return Math.min(availW / root.boundsW, availH / root.boundsH);
                }
                readonly property real offsetX: (width - root.boundsW * viewScale) / 2
                readonly property real offsetY: (height - root.boundsH * viewScale) / 2

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.selectedId = null
                }

                MD.Text {
                    anchors.centerIn: parent
                    visible: (root.rects.length === 0)
                    text: "No displays registered"
                    typescale: MD.Token.typescale.body_medium
                    color: MD.Token.color.on_surface_variant
                }

                Repeater {
                    model: root.rects

                    delegate: Item {
                        id: rectItem
                        required property int index
                        required property var modelData

                        readonly property var d: modelData.d
                        readonly property bool hasLink: (d.links && d.links.length > 0)
                        readonly property bool isSelected: (root.selectedId === d.id)

                        x: canvas.offsetX + modelData.x * canvas.viewScale
                        y: canvas.offsetY + modelData.y * canvas.viewScale
                        width: modelData.w * canvas.viewScale
                        height: modelData.h * canvas.viewScale

                        Shape {
                            anchors.fill: parent
                            preferredRendererType: Shape.CurveRenderer
                            antialiasing: true

                            ShapePath {
                                strokeColor: rectItem.isSelected ? MD.Token.color.primary : MD.Token.color.outline
                                strokeWidth: rectItem.isSelected ? 3 : 1.5
                                fillColor: rectItem.hasLink ? MD.Token.color.primary_container : MD.Token.color.surface_container_highest
                                capStyle: ShapePath.RoundCap
                                joinStyle: ShapePath.RoundJoin

                                PathRectangle {
                                    x: 0
                                    y: 0
                                    width: rectItem.width
                                    height: rectItem.height
                                    radius: 10
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectedId = rectItem.d.id
                        }

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 4

                            MD.Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: rectItem.d.name || ("Display " + rectItem.d.id)
                                typescale: MD.Token.typescale.title_small
                                color: rectItem.hasLink ? MD.Token.color.on_primary_container : MD.Token.color.on_surface
                            }

                            MD.Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: rectItem.d.width + " × " + rectItem.d.height
                                typescale: MD.Token.typescale.label_medium
                                color: rectItem.hasLink ? MD.Token.color.on_primary_container : MD.Token.color.on_surface_variant
                            }
                        }

                        MD.Text {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: 6
                            text: "#" + rectItem.d.id
                            typescale: MD.Token.typescale.label_small
                            color: rectItem.hasLink ? MD.Token.color.on_primary_container : MD.Token.color.on_surface_variant
                        }
                    }
                }
            }
        }

        // --- Inline details panel (squeezes out below canvas) ---
        MD.Pane {
            id: detailsPane
            Layout.fillWidth: true
            Layout.preferredHeight: root.selected ? implicitHeight : 0

            leftPadding: 16
            rightPadding: 16
            bottomPadding: 12

            radius: 16
            backgroundColor: MD.MProp.color.surface
            visible: Layout.preferredHeight > 0.5
            clip: true

            Behavior on Layout.preferredHeight {
                NumberAnimation {
                    duration: 200
                    easing.type: Easing.InOutCubic
                }
            }

            contentItem: ColumnLayout {
                id: detailsContent
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    MD.Text {
                        Layout.fillWidth: true
                        text: root.selected ? (root.selected.name || ("Display " + root.selected.id)) : ""
                        typescale: MD.Token.typescale.title_medium
                        color: MD.Token.color.on_surface
                        elide: Text.ElideRight
                    }

                    MD.IconButton {
                        icon.name: MD.Token.icon.close
                        onClicked: root.selectedId = null
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 24

                    RowLayout {
                        spacing: 8
                        MD.Text {
                            text: "ID:"
                            typescale: MD.Token.typescale.label_medium
                            color: MD.Token.color.on_surface_variant
                        }
                        MD.Text {
                            text: root.selected ? "#" + root.selected.id : ""
                            typescale: MD.Token.typescale.body_medium
                            color: MD.Token.color.on_surface
                        }
                    }

                    RowLayout {
                        spacing: 8
                        MD.Text {
                            text: "Size:"
                            typescale: MD.Token.typescale.label_medium
                            color: MD.Token.color.on_surface_variant
                        }
                        MD.Text {
                            text: root.selected ? root.selected.width + " × " + root.selected.height : ""
                            typescale: MD.Token.typescale.body_medium
                            color: MD.Token.color.on_surface
                        }
                    }

                    RowLayout {
                        visible: !!root.selected && root.selected.refreshMhz > 0
                        spacing: 8
                        MD.Text {
                            text: "Refresh:"
                            typescale: MD.Token.typescale.label_medium
                            color: MD.Token.color.on_surface_variant
                        }
                        MD.Text {
                            text: root.selected ? (root.selected.refreshMhz / 1000).toFixed(3) + " Hz" : ""
                            typescale: MD.Token.typescale.body_medium
                            color: MD.Token.color.on_surface
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }
                }

                MD.Divider {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                }

                MD.Text {
                    text: "Connected renderer"
                    typescale: MD.Token.typescale.title_small
                    color: MD.Token.color.on_surface
                }

                RowLayout {
                    readonly property string connectedId: {
                        if (!root.selected) return "";
                        const links = root.selected.links || [];
                        return links.length > 0 ? (links[0].rendererId || "") : "";
                    }
                    Layout.fillWidth: true
                    spacing: 8

                    MD.Icon {
                        name: parent.connectedId.length > 0
                            ? MD.Token.icon.play_arrow
                            : MD.Token.icon.pause
                        size: 18
                        color: parent.connectedId.length > 0
                            ? MD.Token.color.primary
                            : MD.Token.color.on_surface_variant
                    }
                    MD.Text {
                        Layout.fillWidth: true
                        text: parent.connectedId.length > 0
                            ? parent.connectedId
                            : "Idle — no renderer connected."
                        typescale: MD.Token.typescale.body_small
                        color: parent.connectedId.length > 0
                            ? MD.Token.color.on_surface
                            : MD.Token.color.on_surface_variant
                        font.family: parent.connectedId.length > 0 ? "monospace" : ""
                        elide: Text.ElideMiddle
                    }
                }

                // ---- Layout (fillmode + align) ----
                MD.Divider {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.bottomMargin: 4
                    visible: !!root.selected
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: !!root.selected
                    spacing: 8

                    MD.Text {
                        Layout.fillWidth: true
                        text: "Layout"
                        typescale: MD.Token.typescale.title_small
                        color: MD.Token.color.on_surface
                    }

                    // Single reset: clears every per-display override
                    // for this display in one round-trip. Only shown
                    // when at least one field is actually overridden.
                    MD.IconButton {
                        visible: {
                            if (! root.selected) return false;
                            const ovr = root.selected.layoutOverride || ({});
                            return ovr.fillmodeSet === true
                                || ovr.alignSet === true
                                || ovr.clearRgbaSet === true;
                        }
                        icon.name: MD.Token.icon.refresh
                        T.ToolTip.visible: hovered
                        T.ToolTip.text: "Revert to global default"
                        onClicked: {
                            if (! root.selected) return;
                            layoutSetQuery.name = root.selected.name;
                            layoutSetQuery.fillmodeSet = false;
                            layoutSetQuery.alignSet = false;
                            layoutSetQuery.clearRgbaSet = false;
                            layoutSetQuery.clearFillmode = true;
                            layoutSetQuery.clearAlign = true;
                            layoutSetQuery.clearClearRgba = true;
                            layoutSetQuery.reload();
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: !!root.selected
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        MD.Text {
                            text: "Fill mode"
                            typescale: MD.Token.typescale.label_medium
                            color: MD.Token.color.on_surface_variant
                        }

                        MD.ComboBox {
                            id: fillmodeBox
                            Layout.fillWidth: true
                            model: root.kFillModeLabels
                            currentIndex: {
                                if (! root.selected) return 0;
                                const eff = root.selected.effectiveLayout || ({});
                                return root.fillmodeIndex(eff.fillmode || 0);
                            }
                            onActivated: idx => {
                                if (! root.selected) return;
                                layoutSetQuery.name = root.selected.name;
                                layoutSetQuery.fillmodeSet = true;
                                layoutSetQuery.fillmode = root.kFillModeValues[idx];
                                layoutSetQuery.alignSet = false;
                                layoutSetQuery.clearRgbaSet = false;
                                layoutSetQuery.clearFillmode = false;
                                layoutSetQuery.clearAlign = false;
                                layoutSetQuery.clearClearRgba = false;
                                layoutSetQuery.reload();
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: 4

                        MD.Text {
                            text: "Align"
                            typescale: MD.Token.typescale.label_medium
                            color: MD.Token.color.on_surface_variant
                        }

                        // 3×3 grid of toggle pads. Disabled when the
                        // active fillmode is Stretched (align has no effect).
                        GridLayout {
                            columns: 3
                            rowSpacing: 4
                            columnSpacing: 4
                            enabled: {
                                if (! root.selected) return false;
                                const eff = root.selected.effectiveLayout || ({});
                                // Stretched (1) ignores align.
                                return (eff.fillmode || 0) !== 1;
                            }
                            opacity: enabled ? 1.0 : 0.4

                            Repeater {
                                model: 9
                                delegate: Rectangle {
                                    required property int index

                                    readonly property int alignValue: root.kAlignValues[index]
                                    readonly property bool isCurrent: {
                                        if (! root.selected) return false;
                                        const eff = root.selected.effectiveLayout || ({});
                                        return (eff.align || 0) === alignValue;
                                    }

                                    width: 22
                                    height: 22
                                    radius: 4
                                    color: isCurrent ? MD.Token.color.primary : MD.Token.color.surface_container_highest
                                    border.color: MD.Token.color.outline
                                    border.width: 1

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 6
                                        height: 6
                                        radius: 3
                                        color: parent.isCurrent ? MD.Token.color.on_primary : MD.Token.color.on_surface_variant
                                    }

                                    T.ToolTip.visible: ma.containsMouse
                                    T.ToolTip.text: root.kAlignTooltips[index]

                                    MouseArea {
                                        id: ma
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            if (! root.selected) return;
                                            layoutSetQuery.name = root.selected.name;
                                            layoutSetQuery.fillmodeSet = false;
                                            layoutSetQuery.alignSet = true;
                                            layoutSetQuery.align = parent.alignValue;
                                            layoutSetQuery.clearRgbaSet = false;
                                            layoutSetQuery.clearFillmode = false;
                                            layoutSetQuery.clearAlign = false;
                                            layoutSetQuery.clearClearRgba = false;
                                            layoutSetQuery.reload();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
        Item {}
    }
}
