/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "backgroundmonitor.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInformation>
#include <QSaveFile>
#include <QSettings>

#include "core/backends/lan/lanlinkprovider.h"
#include "core/daemon.h"
#include "core/kdeconnectconfig.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

BackgroundMonitor::BackgroundMonitor(Daemon *daemon)
    : QObject(daemon)
    , m_daemon(daemon)
{
    m_clock.start();
    m_timer.setInterval(10000);
    connect(&m_timer, &QTimer::timeout, this, &BackgroundMonitor::cycle);
    m_timer.start();
    connect(daemon, &Daemon::pairingRequestsChanged, this, [this]() {
        const auto requests = m_daemon->pairingRequests();
        const QSet<QString> pending(requests.begin(), requests.end());
        for (const auto &id : pending - m_pairingRequests) {
            if (const auto *device = m_daemon->getDevice(id)) {
                log(QStringLiteral("Pairing request: %1; verification key: %2; accept manually with kdeconnect-cli").arg(id, device->verificationKey()));
            }
        }
        m_pairingRequests = pending;
    });
    // Daemon initializes its backends on the next event-loop iteration.
    QTimer::singleShot(0, this, [this]() {
        if (auto *network = QNetworkInformation::instance()) {
            const auto reset = [this]() {
                m_schedule.reset();
                cycle();
            };
            connect(network, &QNetworkInformation::reachabilityChanged, this, reset);
            connect(network, &QNetworkInformation::transportMediumChanged, this, reset);
        }
        cycle();
    });
    log(QStringLiteral("Background monitoring started"));
}

BackgroundMonitor::~BackgroundMonitor()
{
#ifdef Q_OS_WIN
    if (m_heartbeat) {
        CloseHandle(m_heartbeat);
    }
    if (m_stop) {
        CloseHandle(m_stop);
    }
#endif
}

bool BackgroundMonitor::supervise(const QString &heartbeatName, const QString &stopName)
{
#ifdef Q_OS_WIN
    m_heartbeat = OpenEventW(EVENT_MODIFY_STATE, FALSE, reinterpret_cast<LPCWSTR>(heartbeatName.utf16()));
    m_stop = OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(stopName.utf16()));
    if (!m_heartbeat || !m_stop) {
        log(QStringLiteral("Cannot open service supervision events"));
        return false;
    }
    m_supervisorTimer.setInterval(2000);
    connect(&m_supervisorTimer, &QTimer::timeout, this, &BackgroundMonitor::checkSupervisor);
    m_supervisorTimer.start();
    checkSupervisor();
    return true;
#else
    Q_UNUSED(heartbeatName)
    Q_UNUSED(stopName)
    return false;
#endif
}

void BackgroundMonitor::cycle()
{
    const QDir directory = KdeConnectConfig::instance().baseConfigDir();
    QSettings config(directory.filePath(QStringLiteral("config")), QSettings::IniFormat);
    QSet<QHostAddress> targets;
    for (const auto &value : config.value(QStringLiteral("customDevices")).toStringList()) {
        const QHostAddress address(value.trimmed());
        if (!address.isNull() && !address.isMulticast() && address != QHostAddress::Any && address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6
            && address != QHostAddress::Broadcast) {
            targets.insert(address);
        }
    }
    QSet<QHostAddress> connected;
    QHash<QHostAddress, QString> deviceIds;
    for (const auto &id : m_daemon->devices(true, true)) {
        if (const auto *device = m_daemon->getDevice(id)) {
            for (const auto &value : device->reachableAddresses()) {
                const QHostAddress address(value);
                connected.insert(address);
                deviceIds.insert(address, id);
            }
        }
    }
    const auto pending = m_schedule.pending(targets, connected, m_clock.elapsed());
    if (!pending.isEmpty()) {
        for (auto *provider : m_daemon->getLinkProviders()) {
            if (auto *lan = qobject_cast<LanLinkProvider *>(provider)) {
                lan->discoverAddresses(pending);
            }
        }
    }
    for (auto it = m_states.begin(); it != m_states.end();) {
        if (!targets.contains(it.key())) {
            it = m_states.erase(it);
        } else {
            ++it;
        }
    }
    QJsonArray devices;
    for (const auto &target : targets) {
        const bool reachable = connected.contains(target);
        const QString state = reachable ? QStringLiteral("Connected") : QStringLiteral("WaitingForConnection");
        if (!m_states.contains(target) || m_states.value(target) != reachable) {
            log(target.toString() + QStringLiteral(" -> ") + state);
            m_states.insert(target, reachable);
        }
        devices.append(QJsonObject{{QStringLiteral("address"), target.toString()},
                                   {QStringLiteral("deviceId"), reachable ? QJsonValue(deviceIds.value(target)) : QJsonValue(QJsonValue::Null)},
                                   {QStringLiteral("state"), state}});
    }
    QSaveFile status(directory.filePath(QStringLiteral("background-status.json")));
    if (status.open(QIODevice::WriteOnly)) {
        const QJsonObject object{{QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                 {QStringLiteral("pid"), QCoreApplication::applicationPid()},
                                 {QStringLiteral("devices"), devices}};
        const QByteArray data = QJsonDocument(object).toJson();
        if (status.write(data) == data.size()) {
            status.commit();
        }
    }
}

void BackgroundMonitor::checkSupervisor()
{
#ifdef Q_OS_WIN
    if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) {
        QCoreApplication::quit();
        return;
    }
    if (m_busProbe) {
        return;
    }
    const auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                        QStringLiteral("/org/freedesktop/DBus"),
                                                        QStringLiteral("org.freedesktop.DBus.Peer"),
                                                        QStringLiteral("Ping"));
    m_busProbe = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 10000), this);
    connect(m_busProbe, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        if (!reply->isError()) {
            SetEvent(m_heartbeat);
        } else {
            log(QStringLiteral("D-Bus health check failed"));
        }
        m_busProbe = nullptr;
        reply->deleteLater();
    });
#endif
}

void BackgroundMonitor::log(const QString &message)
{
    const QString path = KdeConnectConfig::instance().baseConfigDir().filePath(QStringLiteral("background.log"));
    QFile file(path);
    if (file.size() > 1024 * 1024) {
        QFile::remove(path + QStringLiteral(".1"));
        file.rename(path + QStringLiteral(".1"));
        file.setFileName(path);
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + QLatin1Char(' ') + message + QLatin1Char('\n')).toUtf8());
    }
}
