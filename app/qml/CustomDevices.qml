// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kdeconnect
import org.kde.kdeconnect.app

Kirigami.ScrollablePage {
    id: page

    title: i18nc("@title:window", "Custom Devices by IP")
    property string errorMessage: ""

    function saveAddresses(addresses: var): bool {
        DaemonDbusInterface.customDevices = addresses;
        if (JSON.stringify(DaemonDbusInterface.customDevices) !== JSON.stringify(addresses)) {
            errorMessage = i18nc("@info", "Could not save IP addresses. Make sure KDE Connect is running.");
            return false;
        }
        errorMessage = "";
        return true;
    }

    function addAddress(): void {
        if (!addressField.acceptableInput) {
            return;
        }
        const address = addressValidator.normalizedAddress(addressField.text);
        const addresses = DaemonDbusInterface.customDevices.slice();
        if (addresses.some(value => addressValidator.normalizedAddress(value) === address)) {
            errorMessage = i18nc("@info", "This IP address is already in the list.");
            return;
        }
        addresses.push(address);
        if (saveAddresses(addresses)) {
            addressField.clear();
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18nc("@info", "Add the IP address of a device to discover it outside your local network. The address is saved and discovery starts immediately. Pair the device from the device list once it appears.")
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            QQC2.TextField {
                id: addressField
                Layout.fillWidth: true
                placeholderText: i18nc("@info:placeholder", "IPv4 or IPv6 address")
                Accessible.name: i18nc("@label:textbox", "Device IP address")
                validator: IpAddressValidator {
                    id: addressValidator
                }
                onTextEdited: page.errorMessage = ""
                onAccepted: page.addAddress()
            }

            QQC2.Button {
                text: i18nc("@action:button", "Add")
                icon.name: "list-add"
                enabled: addressField.acceptableInput
                onClicked: page.addAddress()
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: page.errorMessage
            visible: text.length > 0
        }

        Repeater {
            model: DaemonDbusInterface.customDevices

            delegate: RowLayout {
                id: addressDelegate

                required property string modelData
                required property int index

                Layout.fillWidth: true

                QQC2.Label {
                    Layout.fillWidth: true
                    text: addressDelegate.modelData
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                }

                QQC2.Button {
                    text: i18nc("@action:button", "Remove")
                    icon.name: "list-remove"
                    Accessible.name: i18nc("@action:button accessible", "Remove %1", addressDelegate.modelData)
                    onClicked: {
                        const addresses = DaemonDbusInterface.customDevices.slice();
                        addresses.splice(addressDelegate.index, 1);
                        page.saveAddresses(addresses);
                    }
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            QQC2.Button {
                text: i18nc("@action:button", "Close")
                onClicked: page.Kirigami.PageStack.closeDialog()
            }
        }
    }

    Component.onCompleted: addressField.forceActiveFocus()
}
