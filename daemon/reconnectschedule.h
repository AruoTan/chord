/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QHash>
#include <QHostAddress>
#include <QSet>

class ReconnectSchedule
{
public:
    QList<QHostAddress> pending(const QSet<QHostAddress> &targets, const QSet<QHostAddress> &connected, qint64 now)
    {
        for (auto it = m_retries.begin(); it != m_retries.end();) {
            if (!targets.contains(it.key()) || connected.contains(it.key())) {
                it = m_retries.erase(it);
            } else {
                ++it;
            }
        }
        QList<QHostAddress> result;
        for (const auto &address : targets) {
            if (connected.contains(address)) {
                continue;
            }
            auto &retry = m_retries[address];
            if (now >= retry.next) {
                result.append(address);
                retry.next = now + retry.delay;
                retry.delay = qMin<qint64>(retry.delay * 2, 120000);
            }
        }
        return result;
    }

    void reset()
    {
        m_retries.clear();
    }

private:
    struct Retry {
        qint64 next = 0;
        qint64 delay = 10000;
    };
    QHash<QHostAddress, Retry> m_retries;
};
