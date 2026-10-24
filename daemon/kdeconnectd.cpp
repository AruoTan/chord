/**
 * SPDX-FileCopyrightText: 2014 Yuri Samoilenko <kinnalru@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDBusMessage>
#include <QIcon>
#include <QLocale>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#include <KAboutData>
#include <KColorSchemeManager>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>

#include <dbushelper.h>

#include "backgroundmonitor.h"
#include "desktop_daemon.h"
#include "kdeconnect-version.h"
#include <core/processhelper.h>
#ifdef Q_OS_WIN
#include "core/windowsservice.h"
#endif

// Copied from plasma-workspace/libkworkspace/kworkspace.cpp
static void detectPlatform(int argc, char **argv)
{
    if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        if (qstrcmp(argv[i], "-platform") == 0 || qstrcmp(argv[i], "--platform") == 0 || QByteArray(argv[i]).startsWith("-platform=")
            || QByteArray(argv[i]).startsWith("--platform=")) {
            return;
        }
    }
    const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
    if (sessionType.isEmpty()) {
        return;
    }
    if (qstrcmp(sessionType.constData(), "wayland") == 0) {
        qputenv("QT_QPA_PLATFORM", "wayland");
    } else if (qstrcmp(sessionType.constData(), "x11") == 0) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
}

int main(int argc, char *argv[])
{
    qputenv("LANGUAGE", "en_US");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
#ifdef Q_OS_WIN
    // If ran from a console, redirect the output there
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif

    detectPlatform(argc, argv);
    QGuiApplication::setQuitLockEnabled(false);

    // kdeconnectd is autostarted, so disable session management to speed up startup
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);

    // Don't have Solid backend talk to ourself.
    qputenv("SOLID_DISABLE_KDECONNECT", "1");

    QApplication app(argc, argv);
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
    KAboutData aboutData(QStringLiteral("kdeconnect.daemon"),
                         i18n("KDE Connect Daemon"),
                         QStringLiteral(KDECONNECT_VERSION_STRING),
                         i18n("KDE Connect Daemon"),
                         KAboutLicense::GPL,
                         i18n("© 2015–2025 KDE Connect Team"));
    KAboutData::setApplicationData(aboutData);
    app.setQuitOnLastWindowClosed(false);

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // Ensure we have a suitable color theme set for light/dark mode. KColorSchemeManager implicitly applies
    // a suitable default theme.
    KColorSchemeManager::instance();
    // Force breeze style to ensure coloring works consistently in dark mode. Specifically tab colors have
    // troubles on windows.
    QApplication::setStyle(QStringLiteral("breeze"));
    // Force breeze icon theme to ensure we can correctly adapt icons to color changes WRT dark/light mode.
    // Without this we may end up with hicolor and fail to support icon recoloring.
    QIcon::setThemeName(QStringLiteral("breeze"));
#else
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
#endif

    // Default to org.kde.desktop style unless the user forces another style
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    KCrash::initialize();

    QCommandLineParser parser;
#ifdef Q_OS_WIN
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    QCommandLineOption embeddingOption(QStringLiteral("Embedding"));
    embeddingOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(embeddingOption);
#endif
    QCommandLineOption replaceOption({QStringLiteral("replace")}, i18n("Replace an existing instance"));
    parser.addOption(replaceOption);
    QCommandLineOption backgroundOption(QStringLiteral("background"), i18n("Run background monitoring without opening the app or indicator"));
    parser.addOption(backgroundOption);
    QCommandLineOption heartbeatOption(QStringLiteral("service-heartbeat"), QString(), QStringLiteral("event"));
    QCommandLineOption stopOption(QStringLiteral("service-stop"), QString(), QStringLiteral("event"));
    heartbeatOption.setFlags(QCommandLineOption::HiddenFromHelp);
    stopOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(heartbeatOption);
    parser.addOption(stopOption);
    aboutData.setupCommandLine(&parser);

    parser.process(app);

    aboutData.processCommandLine(&parser);

    bool background = parser.isSet(backgroundOption);
#ifdef Q_OS_WIN
    if (WindowsService::removalInProgressForCurrentUser()) {
        qWarning() << "Background service removal is in progress; finish --service-unregister before starting KDE Connect";
        return 1;
    }
    background = background || parser.isSet(embeddingOption) || WindowsService::registeredForCurrentUser();
    if (parser.isSet(embeddingOption)) {
        FreeConsole();
    }
#endif
    if (parser.isSet(heartbeatOption) != parser.isSet(stopOption) || (parser.isSet(heartbeatOption) && !background)) {
        qCritical() << "Service supervision requires background mode and both event names";
        return 1;
    }
    app.setProperty("kdeconnectBackground", background);

    KDBusService::StartupOptions flags = KDBusService::Unique;
    if (parser.isSet(replaceOption)) {
        flags |= KDBusService::Replace;
    }
    KDBusService dbusService(flags);

    DesktopDaemon daemon;

    if (background) {
        auto *monitor = new BackgroundMonitor(&daemon);
        if (parser.isSet(heartbeatOption) && !monitor->supervise(parser.value(heartbeatOption), parser.value(stopOption))) {
            return 1;
        }
    }

#ifdef Q_OS_WIN
    // Show the tray only for an interactive daemon.
    if (!background) {
        ProcessHelper::startDetached(QStringLiteral("kdeconnect-indicator.exe"));
    }
#endif

    return app.exec();
}
