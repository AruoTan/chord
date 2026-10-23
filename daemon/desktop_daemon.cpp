/**
 * SPDX-FileCopyrightText: 2024 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "desktop_daemon.h"

#include <QApplication>
#include <QIcon>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include <KDBusService>
#include <KIO/Global>
#include <KIO/JobTracker>
#include <KLocalizedString>
#include <KNotification>

#include "core/backends/pairinghandler.h"
#include "core/device.h"
#include "core/openconfig.h"

#ifdef Q_OS_WIN
#include "transfernotificationtracker.h"
#endif

DesktopDaemon::DesktopDaemon(QObject *parent)
    : Daemon(parent)
{
    qApp->setWindowIcon(QIcon::fromTheme(QStringLiteral("kdeconnect")));
#ifdef Q_OS_WIN
    m_transferTracker = createWindowsTransferTracker(this);
#endif
}

void DesktopDaemon::askPairingConfirmation(Device *device)
{
    if (qApp->property("kdeconnectBackground").toBool()) {
        qInfo() << "Pairing request from" << device->name() << device->id() << "key:" << device->verificationKey()
                << "- use kdeconnect-cli --device <id> --pair to accept";
        return;
    }
    KNotification *notification = new KNotification(QStringLiteral("pairingRequest"), KNotification::NotificationFlag::Persistent);
    QTimer::singleShot(PairingHandler::pairingTimeoutMsec, notification, &KNotification::close);
    notification->setIconName(QStringLiteral("dialog-information"));
    notification->setComponentName(QStringLiteral("kdeconnect"));
    notification->setTitle(QStringLiteral("KDE Connect"));
    notification->setText(i18n("Pairing request from %1\nKey: %2", device->name().toHtmlEscaped(), device->verificationKey()));
    QString deviceId = device->id();
    auto openSettings = [deviceId, notification] {
        OpenConfig oc;
        oc.setXdgActivationToken(notification->xdgActivationToken());
        oc.openConfiguration(deviceId);
    };

    KNotificationAction *openSettingsAction = notification->addDefaultAction(i18n("Open"));
    connect(openSettingsAction, &KNotificationAction::activated, notification, openSettings);

    KNotificationAction *acceptAction = notification->addAction(i18n("Accept"));
    connect(acceptAction, &KNotificationAction::activated, device, &Device::acceptPairing);

    KNotificationAction *rejectAction = notification->addAction(i18n("Reject"));
    connect(rejectAction, &KNotificationAction::activated, device, &Device::cancelPairing);

    int previousPairState = device->pairStateAsInt();
    connect(device, &Device::pairStateChanged, notification, [notification, previousPairState](int pairState) {
        if (pairState != previousPairState) {
            notification->close();
        }
    });

    notification->sendEvent();
}

void DesktopDaemon::reportError(const QString &title, const QString &text)
{
    qWarning() << title << ":" << text;
    sendSimpleNotification(QStringLiteral("error"), title, text, QStringLiteral("dialog-error"));
}

KJobTrackerInterface *DesktopDaemon::jobTracker()
{
#ifdef Q_OS_WIN
    return m_transferTracker;
#else
    return KIO::getJobTracker();
#endif
}

// NOTE: eventId must match a notificatiion event id defined in plugins/kdeconnect.notifyrc
Q_SCRIPTABLE void DesktopDaemon::sendSimpleNotification(const QString &eventId, const QString &title, const QString &text, const QString &iconName)
{
    KNotification *notification = new KNotification(eventId);
    notification->setIconName(iconName);
    notification->setComponentName(QStringLiteral("kdeconnect"));
    notification->setTitle(title);
    notification->setText(text);
    notification->sendEvent();
}

void DesktopDaemon::quit()
{
    QApplication::quit();
}
