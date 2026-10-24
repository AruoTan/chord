/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "transfernotificationtracker.h"

#include <KFormat>
#include <KLocalizedString>
#include <QFileInfo>
#include <QUuid>
#include <utility>

TransferNotificationTracker::TransferNotificationTracker(Notify notify, QObject *parent)
    : KJobTrackerInterface(parent)
    , m_notify(std::move(notify))
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        for (auto it = m_transfers.begin(); it != m_transfers.end(); ++it) {
            if (it->dirty) {
                publish(it.key(), it.value(), false);
            }
        }
    });
}

void TransferNotificationTracker::registerJob(KJob *job)
{
    if (m_transfers.contains(job)) {
        return;
    }
    Transfer transfer;
    transfer.notification.tag = QUuid::createUuid().toString(QUuid::Id128).left(16);
    m_transfers.insert(job, transfer);
    connect(job, &KJob::finished, this, [this](KJob *job) {
        auto it = m_transfers.find(job);
        if (it != m_transfers.end()) {
            publish(job, it.value(), true);
        }
    });
    KJobTrackerInterface::registerJob(job);
    if (!m_timer.isActive()) {
        m_timer.start();
    }
}

void TransferNotificationTracker::unregisterJob(KJob *job)
{
    KJobTrackerInterface::unregisterJob(job);
    m_transfers.remove(job);
    if (m_transfers.isEmpty()) {
        m_timer.stop();
    }
}

void TransferNotificationTracker::description(KJob *job, const QString &, const QPair<QString, QString> &source, const QPair<QString, QString> &destination)
{
    auto it = m_transfers.find(job);
    if (it == m_transfers.end()) {
        return;
    }
    it->source = source.second;
    it->destination = QFileInfo(destination.second).fileName();
    it->phase = i18n("Receiving...");
    it->speed = 0;
    it->dirty = true;
    if (!it->shown) {
        publish(job, it.value(), false);
    }
}

void TransferNotificationTracker::infoMessage(KJob *job, const QString &message)
{
    auto it = m_transfers.find(job);
    if (it != m_transfers.end()) {
        it->phase = message;
        it->speed = 0;
        it->dirty = true;
    }
}

void TransferNotificationTracker::totalAmount(KJob *job, KJob::Unit, qulonglong)
{
    auto it = m_transfers.find(job);
    if (it != m_transfers.end()) {
        it->dirty = true;
    }
}

void TransferNotificationTracker::processedAmount(KJob *job, KJob::Unit unit, qulonglong amount)
{
    totalAmount(job, unit, amount);
}

void TransferNotificationTracker::speed(KJob *job, unsigned long value)
{
    auto it = m_transfers.find(job);
    if (it != m_transfers.end()) {
        it->speed = value;
        it->dirty = true;
    }
}

void TransferNotificationTracker::publish(KJob *job, Transfer &transfer, bool final)
{
    auto &notification = transfer.notification;
    const KFormat format;
    const auto received = job->processedAmount(KJob::Bytes);
    const auto total = job->totalAmount(KJob::Bytes);
    const auto files = job->totalAmount(KJob::Files);
    const auto completed = job->processedAmount(KJob::Files);
    ++notification.sequence;
    notification.finished = final;
    notification.title = final ? (job->error() ? i18n("File transfer failed") : i18n("File transfer complete")) : i18n("Receiving files");
    notification.text = transfer.source + QStringLiteral("\n") + transfer.destination;
    notification.status = transfer.phase.isEmpty() ? i18n("Receiving...") : transfer.phase;
    notification.progress = total ? qMin(0.99, double(received) / double(total)) : -1;
    notification.progressText = total ? i18n("%1 / %2", format.formatByteSize(received), format.formatByteSize(total)) : format.formatByteSize(received);
    if (files) {
        notification.progressText += i18n(" - %1 / %2 entries", completed, files);
    }
    if (notification.progress >= 0) {
        notification.progressText = i18n("%1% - %2", int(notification.progress * 100), notification.progressText);
    }
    if (transfer.speed) {
        notification.status += i18n(" - %1/s", format.formatByteSize(transfer.speed));
    }
    if (final) {
        if (job->error() == KJob::KilledJobError) {
            notification.title = i18n("File transfer canceled");
        }
        notification.progress = job->error() ? notification.progress : 1;
        notification.status = job->error() == KJob::KilledJobError ? i18n("Transfer canceled") : notification.title;
        if (job->error()) {
            notification.text += QStringLiteral("\n") + job->errorText();
        }
    }
    const bool accepted = m_notify(notification, !transfer.shown || final);
    transfer.shown = transfer.shown || accepted;
    transfer.dirty = !accepted;
}
