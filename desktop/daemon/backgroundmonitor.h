/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "reconnectschedule.h"

class Daemon;
class QDBusPendingCallWatcher;

class BackgroundMonitor : public QObject
{
    Q_OBJECT
public:
    explicit BackgroundMonitor(Daemon *daemon);
    ~BackgroundMonitor() override;

    bool supervise(const QString &heartbeatName, const QString &stopName);

private:
    void cycle();
    void checkSupervisor();
    void log(const QString &message);

    Daemon *m_daemon;
    QTimer m_timer;
    QTimer m_supervisorTimer;
    QElapsedTimer m_clock;
    ReconnectSchedule m_schedule;
    QHash<QHostAddress, bool> m_states;
    QSet<QString> m_pairingRequests;
    QDBusPendingCallWatcher *m_busProbe = nullptr;
    void *m_heartbeat = nullptr;
    void *m_stop = nullptr;
};
