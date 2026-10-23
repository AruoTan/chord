/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "daemon/transfernotificationtracker.h"

#include <KLocalizedString>
#include <QTest>

class TransferJob : public KJob
{
public:
    TransferJob()
    {
        setAutoDelete(false);
        setCapabilities(Killable);
    }
    void start() override
    {
        Q_EMIT description(this,
                           QStringLiteral("Receiving files"),
                           {QStringLiteral("Source"), QStringLiteral("Phone")},
                           {QStringLiteral("Destination"), QStringLiteral("/downloads/folder/file.txt")});
    }
    void progress(qulonglong received, qulonglong total)
    {
        setTotalAmount(Bytes, total);
        setProcessedAmount(Bytes, received);
        emitSpeed(1024);
    }
    void verify()
    {
        Q_EMIT infoMessage(this, QStringLiteral("Verifying SHA-256..."));
    }
    void complete(bool fail = false)
    {
        if (fail) {
            setError(UserDefinedError);
            setErrorText(QStringLiteral("SHA-256 verification failed: file.txt"));
        }
        emitResult();
    }

protected:
    bool doKill() override
    {
        return true;
    }
};

class TransferNotificationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("kdeconnect-test");
        KLocalizedString::setLanguages({QStringLiteral("en_US")});
    }

    void progressAndVerification()
    {
        QList<TransferNotification> notifications;
        QList<bool> replacements;
        TransferNotificationTracker tracker([&](const TransferNotification &notification, bool replace) {
            notifications.append(notification);
            replacements.append(replace);
        });
        TransferJob job;
        tracker.registerJob(&job);
        tracker.registerJob(&job);
        job.start();
        QCOMPARE(notifications.size(), 1);
        QVERIFY(replacements.first());
        QCOMPARE(notifications.first().progress, -1);
        QVERIFY(notifications.first().text.contains(QStringLiteral("Phone")));
        QVERIFY(notifications.first().text.contains(QStringLiteral("file.txt")));
        for (int bytes = 1; bytes <= 50; ++bytes) {
            job.progress(bytes, 100);
        }
        QCOMPARE(notifications.size(), 1);
        QTRY_COMPARE(notifications.size(), 2);
        QCOMPARE(notifications.last().progress, 0.5);
        QVERIFY(!replacements.last());
        QVERIFY(notifications.last().status.contains(QStringLiteral("/s")));
        job.progress(100, 100);
        job.verify();
        QTRY_COMPARE(notifications.size(), 3);
        QVERIFY(notifications.last().progress < 1);
        QCOMPARE(notifications.last().status, QStringLiteral("Verifying SHA-256..."));
        QVERIFY(!notifications.last().finished);
        job.complete();
        QCOMPARE(notifications.size(), 4);
        QVERIFY(notifications.last().finished);
        QCOMPARE(notifications.last().progress, 1);
        QVERIFY(replacements.last());
        QCOMPARE(notifications.first().tag, notifications.last().tag);
        QCOMPARE(notifications.last().sequence, 4);
        QTest::qWait(600);
        QCOMPARE(notifications.size(), 4);
    }

    void failureAndConcurrentTransfers()
    {
        QList<TransferNotification> notifications;
        TransferNotificationTracker tracker([&](const TransferNotification &notification, bool) {
            notifications.append(notification);
        });
        TransferJob first;
        TransferJob second;
        tracker.registerJob(&first);
        tracker.registerJob(&second);
        first.start();
        second.start();
        const QString firstTag = notifications.at(0).tag;
        const QString secondTag = notifications.at(1).tag;
        QVERIFY(firstTag != secondTag);
        first.progress(100, 100);
        first.complete(true);
        QCOMPARE(notifications.last().tag, firstTag);
        QVERIFY(notifications.last().finished);
        QVERIFY(notifications.last().progress < 1);
        QVERIFY(notifications.last().text.contains(QStringLiteral("SHA-256 verification failed")));
        second.progress(50, 100);
        QTRY_COMPARE(notifications.last().tag, secondTag);
        QVERIFY(!notifications.last().finished);
        QVERIFY(second.kill(KJob::Quietly));
        QVERIFY(notifications.last().finished);
        QCOMPARE(notifications.last().status, QStringLiteral("Transfer canceled"));
    }

    void emptyTransferAndUnregister()
    {
        QList<TransferNotification> notifications;
        TransferNotificationTracker tracker([&](const TransferNotification &notification, bool) {
            notifications.append(notification);
        });
        TransferJob empty;
        tracker.registerJob(&empty);
        empty.start();
        empty.verify();
        empty.complete();
        QCOMPARE(notifications.size(), 2);
        QCOMPARE(notifications.last().progress, 1);
        QVERIFY(notifications.last().finished);

        TransferJob detached;
        tracker.registerJob(&detached);
        detached.start();
        tracker.unregisterJob(&detached);
        const auto count = notifications.size();
        detached.complete();
        QTest::qWait(600);
        QCOMPARE(notifications.size(), count);
    }
};

QTEST_GUILESS_MAIN(TransferNotificationTest)

#include "transfernotificationtest.moc"
