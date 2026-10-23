/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QTest>

#include "daemon/reconnectschedule.h"

class ReconnectScheduleTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void backoffAndRecovery()
    {
        ReconnectSchedule schedule;
        const QHostAddress address(QStringLiteral("192.0.2.1"));
        const QSet<QHostAddress> targets{address};
        QCOMPARE(schedule.pending(targets, {}, 0), QList<QHostAddress>{address});
        QVERIFY(schedule.pending(targets, {}, 9999).isEmpty());
        QCOMPARE(schedule.pending(targets, {}, 10000).size(), 1);
        QVERIFY(schedule.pending(targets, {}, 29999).isEmpty());
        QCOMPARE(schedule.pending(targets, {}, 30000).size(), 1);
        QVERIFY(schedule.pending(targets, targets, 31000).isEmpty());
        // A real connection resets backoff; a later disconnect retries immediately.
        QCOMPARE(schedule.pending(targets, {}, 32000).size(), 1);
        QCOMPARE(schedule.pending(targets, {}, 42000).size(), 1);
    }

    void addressChangesAndRemoval()
    {
        ReconnectSchedule schedule;
        const QHostAddress oldAddress(QStringLiteral("192.0.2.1"));
        const QHostAddress newAddress(QStringLiteral("192.0.2.2"));
        const QSet<QHostAddress> targets{oldAddress, newAddress};
        QCOMPARE(schedule.pending(targets, {oldAddress}, 0), QList<QHostAddress>{newAddress});
        // The same device's new address must not mark its old address connected.
        QCOMPARE(schedule.pending(targets, {newAddress}, 1), QList<QHostAddress>{oldAddress});
        QVERIFY(schedule.pending({}, {}, 2).isEmpty());
        QCOMPARE(schedule.pending({oldAddress}, {}, 3), QList<QHostAddress>{oldAddress});
    }

    void cappedRetriesAndNetworkChange()
    {
        ReconnectSchedule schedule;
        const QHostAddress address(QStringLiteral("2001:db8::1"));
        const QSet<QHostAddress> targets{address};
        qint64 now = 0;
        for (const qint64 interval : {10000, 20000, 40000, 80000, 120000, 120000}) {
            QCOMPARE(schedule.pending(targets, {}, now).size(), 1);
            QVERIFY(schedule.pending(targets, {}, now + interval - 1).isEmpty());
            now += interval;
        }
        schedule.reset();
        QCOMPARE(schedule.pending(targets, {}, now - 1).size(), 1);
    }
};

QTEST_GUILESS_MAIN(ReconnectScheduleTest)

#include "reconnectscheduletest.moc"
