import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    width: 640; height: 480
    title: "Set up schema"

    Component.onCompleted: if (wizard.screen === "confirming") wizard.continueClicked()

    Timer {
        interval: 2000; repeat: true
        // summary: the wizard autostarts at login, before the doctor's heal run writes its status
        running: wizard.screen === "waiting_reboot"
                 || (wizard.screen === "summary" && wizard.statusItems.length === 0)
        onTriggered: wizard.refresh()
    }

    function screenText() {
        switch (wizard.screen) {
        case "welcome": return "Welcome — this will set up schema on your computer.";
        case "waiting_reboot": return "Please restart your computer to continue.";
        case "summary": return "First stage done. Here is what the doctor checked.";
        case "final": return "Schema is successfully installed.";
        case "rolled_back": return "Your computer put itself back safely. The switch was undone, and nothing is broken.";
        case "dbus_offer": return "One last optional step: use schema's own message bus — the channel your desktop and system services talk over. If anything looks wrong after the restart, your computer puts itself back automatically.";
        case "confirming": return "Checking the message bus switch…";
        default: return "";
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 18
            text: win.screenText()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: wizard.detail !== ""
            text: wizard.detail
        }

        ScrollView {
            id: results
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: wizard.screen === "summary"
            clip: true

            ColumnLayout {
                width: results.availableWidth
                spacing: 6

                Label {
                    text: "Overall: " + wizard.overall
                    font.bold: true
                }
                Label {
                    visible: wizard.statusItems.length === 0
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    text: "The doctor hasn't reported yet."
                }
                Repeater {
                    model: wizard.statusItems
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 12; height: 12; radius: 6
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 3
                            color: modelData.color === "GREEN" ? "#27ae60"
                                 : modelData.color === "AMBER" ? "#e67e22"
                                 : modelData.color === "RED" ? "#c0392b" : "#95a5a6"
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: modelData.name
                                  + (modelData.detail ? " — " + modelData.detail : "")
                                  + (modelData.action ? "\n" + modelData.action : "")
                        }
                    }
                }
            }
        }

        // Recovery-card acknowledgement, shown only before the first reboot.
        ColumnLayout {
            Layout.fillWidth: true
            visible: wizard.screen === "welcome"
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: wizard.recoveryText()
            }
            CheckBox {
                id: ack
                text: "I've saved these instructions"
                onToggled: wizard.setRecoveryAck(checked)
            }
        }

        Item { Layout.fillHeight: true; visible: wizard.screen !== "summary" }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: wizard.error !== ""
            color: "#c0392b"
            text: wizard.error
        }

        Button {
            text: wizard.secondaryAction
            visible: wizard.secondaryAction !== ""
            onClicked: wizard.secondaryClicked()
        }

        Button {
            text: wizard.primaryAction
            visible: wizard.primaryAction !== ""
            enabled: wizard.screen !== "welcome" || ack.checked
            onClicked: wizard.continueClicked()
        }
    }
}
