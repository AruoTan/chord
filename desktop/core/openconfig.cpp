/**
 * SPDX-FileCopyrightText: 2022 Nicolas Fella <nicolas.fella@gmx.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "openconfig.h"
#include "processhelper.h"

#include <QCoreApplication>
#include <QDebug>
#include <QVariant>

#include <KIO/CommandLauncherJob>

void OpenConfig::setXdgActivationToken(const QString &token)
{
    m_currentToken = token;
}

void OpenConfig::openConfiguration(const QString &deviceId, const QString &pluginId)
{
    if (QCoreApplication::instance()->property("kdeconnectBackground").toBool()) {
        qInfo() << "App launch suppressed in background mode";
        return;
    }
    QStringList args;

    QString argument;

    if (!deviceId.isEmpty()) {
        args << QStringLiteral("--device");
        args << deviceId;

        if (!pluginId.isEmpty()) {
            args << QStringLiteral("--plugin-config");
            args << pluginId;
        }
    }

#if defined(Q_OS_WIN)
    ProcessHelper::startDetached(QStringLiteral("kdeconnect-app.exe"), args);
#elif defined(Q_OS_MAC)
    ProcessHelper::startDetached(QCoreApplication::applicationDirPath() + QLatin1String("/kdeconnect-app"), args);
#else
    auto job = new KIO::CommandLauncherJob(QStringLiteral("kdeconnect-app"), args);
    job->setDesktopName(QStringLiteral("org.kde.kdeconnect.app"));
    job->setStartupId(m_currentToken.toUtf8());
    job->start();

    m_currentToken = QString();
#endif
}

#include "moc_openconfig.cpp"
