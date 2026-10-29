// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtCore
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.kdeconnect

Kirigami.ScrollablePage {
    id: page

    required property string deviceId
    property var selectedUrls: []
    readonly property var shareIface: ShareDbusInterfaceFactory.create(deviceId)

    title: i18nc("@title:window", "Share Files and Folders")

    function addUrls(urls: var): void {
        const selection = selectedUrls.slice();
        for (const url of urls) {
            const value = url.toString();
            if (!selection.includes(value)) {
                selection.push(value);
            }
        }
        selectedUrls = selection;
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18nc("@info", "Add files and folders to send together. Folders keep their contents and directory structure. Both devices must support folder transfers.")
            wrapMode: Text.WordWrap
        }

        RowLayout {
            QQC2.Button {
                text: i18nc("@action:button", "Add Files")
                icon.name: "document-new"
                onClicked: fileDialog.open()
            }
            QQC2.Button {
                text: i18nc("@action:button", "Add Folder")
                icon.name: "folder-new"
                onClicked: folderDialog.open()
            }
        }

        Repeater {
            model: page.selectedUrls

            delegate: RowLayout {
                id: entry
                required property string modelData
                required property int index
                Layout.fillWidth: true

                QQC2.Label {
                    Layout.fillWidth: true
                    text: decodeURIComponent(entry.modelData)
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                }
                QQC2.Button {
                    text: i18nc("@action:button", "Remove")
                    icon.name: "list-remove"
                    onClicked: {
                        const selection = page.selectedUrls.slice();
                        selection.splice(entry.index, 1);
                        page.selectedUrls = selection;
                    }
                }
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: i18nc("@title:window", "Choose Files")
        currentFolder: StandardPaths.standardLocations(StandardPaths.HomeLocation)[0]
        fileMode: FileDialog.OpenFiles
        onAccepted: page.addUrls(selectedFiles)
    }

    FolderDialog {
        id: folderDialog
        title: i18nc("@title:window", "Choose Folder")
        currentFolder: StandardPaths.standardLocations(StandardPaths.HomeLocation)[0]
        onAccepted: page.addUrls([selectedFolder])
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            QQC2.Button {
                text: i18nc("@action:button", "Cancel")
                onClicked: page.Kirigami.PageStack.closeDialog()
            }
            QQC2.Button {
                text: i18nc("@action:button", "Send")
                icon.name: "document-send"
                enabled: page.selectedUrls.length > 0
                onClicked: {
                    page.shareIface.shareUrls(page.selectedUrls);
                    page.Kirigami.PageStack.closeDialog();
                }
            }
        }
    }
}
