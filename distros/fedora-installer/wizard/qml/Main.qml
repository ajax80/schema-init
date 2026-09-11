import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    width: 640; height: 480
    title: "Set up schema"

    function screenText() {
        switch (wizard.screen) {
        case "welcome": return "Welcome — this will set up schema on your computer.";
        case "waiting_reboot": return "Please restart your computer to continue.";
        case "summary": return "First stage done. Here is what the doctor checked.";
        case "final": return "Schema is successfully installed.";
        case "rolled_back": return "Your computer put itself back safely.";
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

        Item { Layout.fillHeight: true }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: wizard.error !== ""
            color: "#c0392b"
            text: wizard.error
        }

        Button {
            text: wizard.primaryAction
            visible: wizard.primaryAction !== ""
            enabled: wizard.screen !== "welcome" || ack.checked
            onClicked: wizard.continueClicked()
        }
    }
}
