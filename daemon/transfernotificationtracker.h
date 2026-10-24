/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <KJobTrackerInterface>
#include <QHash>
#include <QTimer>
#include <functional>

struct TransferNotification {
    QString tag;
    QString title;
    QString text;
    QString status;
    QString progressText;
    double progress = -1;
    quint32 sequence = 0;
    bool finished = false;
};

class TransferNotificationTracker : public KJobTrackerInterface
{
public:
    // Return false to retry delivery while the transfer is active.
    using Notify = std::function<bool(const TransferNotification &, bool replace)>;
    explicit TransferNotificationTracker(Notify notify, QObject *parent = nullptr);
    void registerJob(KJob *job) override;
    void unregisterJob(KJob *job) override;

protected:
    void description(KJob *job, const QString &title, const QPair<QString, QString> &source, const QPair<QString, QString> &destination) override;
    void infoMessage(KJob *job, const QString &message) override;
    void totalAmount(KJob *job, KJob::Unit unit, qulonglong amount) override;
    void processedAmount(KJob *job, KJob::Unit unit, qulonglong amount) override;
    void speed(KJob *job, unsigned long value) override;

private:
    struct Transfer {
        TransferNotification notification;
        QString source;
        QString destination;
        QString phase;
        unsigned long speed = 0;
        bool shown = false;
        bool dirty = true;
    };
    void publish(KJob *job, Transfer &transfer, bool final);
    QHash<KJob *, Transfer> m_transfers;
    Notify m_notify;
    QTimer m_timer;
};

#ifdef Q_OS_WIN
KJobTrackerInterface *createWindowsTransferTracker(QObject *parent);
#endif
