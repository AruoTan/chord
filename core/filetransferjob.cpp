/*
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 * SPDX-FileCopyrightText: 2015 Aleix Pol i Gonzalez <aleixpol@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "filetransferjob.h"
#include "daemon.h"
#include <core_debug.h>

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QtConcurrentRun>
#include <qalgorithms.h>

#include <KFileUtils>
#include <KLocalizedString>

FileTransferJob::FileTransferJob(const NetworkPacket *np, const QUrl &destination)
    : KJob()
    , m_origin(np->payload())
    , m_reply(nullptr)
    , m_from(QStringLiteral("KDE Connect"))
    , m_destination(destination)
    , m_written(0)
    , m_size(np->payloadSize())
    , m_np(np)
    , m_autoRename(false)
    , m_expectedChecksum(np->get<QString>(QStringLiteral("sha256")).toLatin1())
{
    // m_origin can be null when the packet has no payload (e.g. an empty file): we still want to
    // go through the same job so it gets counted, auto-renamed and timestamped like any other file.
    // Disabled this assert: QBluetoothSocket doesn't report "->isReadable() == true" until it's connected
    // Q_ASSERT(m_origin->isReadable());
    if (m_destination.scheme().isEmpty()) {
        qCWarning(KDECONNECT_CORE) << "Destination QUrl" << m_destination << "lacks a scheme. Setting its scheme to 'file'.";
        m_destination.setScheme(QStringLiteral("file"));
    }

    setCapabilities(Killable);
    qCDebug(KDECONNECT_CORE) << "FileTransferJob Downloading payload to" << destination << "size:" << m_size;
}

void FileTransferJob::start()
{
    if (m_destination.isLocalFile() && QFile::exists(m_destination.toLocalFile()) && !m_directory) {
        if (m_autoRename) {
            QFileInfo fileInfo(m_destination.toLocalFile());
            QString path = fileInfo.path();
            QString fileName = fileInfo.fileName();
            m_destination.setPath(path + QStringLiteral("/") + KFileUtils::suggestName(QUrl::fromLocalFile(path), fileName), QUrl::DecodedMode);
        } else {
            setError(2);
            setErrorText(i18n("Filename already present"));
            emitResult();
            return;
        }
    }
    QMetaObject::invokeMethod(this, "doStart", Qt::QueuedConnection);
    // qCDebug(KDECONNECT_CORE) << "FileTransferJob start";
}

void FileTransferJob::doStart()
{
    if (isFinished()) {
        return;
    }
    if (!isSafeDestination()) {
        setError(UserDefinedError);
        setErrorText(i18n("Unsafe destination path: %1", m_destination.toLocalFile()));
        emitResult();
        return;
    }
    if (m_directory) {
        if (!QDir().mkpath(m_destination.toLocalFile())) {
            setError(UserDefinedError);
            setErrorText(i18n("Could not create destination folder: %1", m_destination.toLocalFile()));
        }
        emitResult();
        return;
    }
    if (!m_origin && m_size != 0) {
        setError(UserDefinedError);
        setErrorText(i18n("Missing file payload from: %1", m_from));
        emitResult();
        return;
    }
    if (!m_origin || m_size == 0) {
        // No payload to transfer (e.g. an empty file): just create the destination file.
        QFile file(m_destination.toLocalFile());
        if (!file.open(QIODevice::WriteOnly)) {
            setError(1);
            setErrorText(i18n("Could not create destination file: %1", m_destination.toLocalFile()));
        }
        file.close();
        verifyChecksum();
        return;
    }

    connect(m_origin.data(), &QIODevice::readyRead, this, &FileTransferJob::startTransfer);
    connect(m_origin.data(), &QIODevice::readChannelFinished, this, &FileTransferJob::sourceFinished);
    if (m_origin->bytesAvailable()) {
        startTransfer();
    } else if (auto *socket = qobject_cast<QAbstractSocket *>(m_origin.data()); socket && socket->state() == QAbstractSocket::UnconnectedState) {
        sourceFinished();
    }
}

void FileTransferJob::sourceFinished()
{
    if (m_reply || isFinished()) {
        return;
    }
    if (m_origin->bytesAvailable()) {
        startTransfer();
    } else if (m_size > 0) {
        setError(UserDefinedError);
        setErrorText(i18n("Received incomplete file from: %1", m_from));
        emitResult();
    } else {
        m_size = 0;
        doStart();
    }
}

void FileTransferJob::startTransfer()
{
    // Don't put each ready read
    if (m_reply || isFinished())
        return;

    setProcessedAmount(Bytes, 0);

    QNetworkRequest req(m_destination);
    if (m_size >= 0) {
        setTotalAmount(Bytes, m_size);
        req.setHeader(QNetworkRequest::ContentLengthHeader, m_size);
    }
    m_reply = Daemon::instance()->networkAccessManager()->put(req, m_origin.data());

    connect(m_reply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64 /*bytesTotal*/) {
        if (!m_timer.isValid())
            m_timer.start();
        setProcessedAmount(Bytes, bytesSent);

        const auto elapsed = m_timer.elapsed();
        if (elapsed > 0) {
            emitSpeed((1000 * bytesSent) / elapsed);
        }

        m_written = bytesSent;
    });
    connect(m_reply, &QNetworkReply::finished, this, &FileTransferJob::transferFinished);
}

void FileTransferJob::transferFinished()
{
    if (isFinished()) {
        return;
    }
    if (m_reply->error()) {
        qCDebug(KDECONNECT_CORE) << "Couldn't transfer the file successfully" << m_reply->error() << m_reply->errorString();
        setError(m_reply->error());
        setErrorText(m_reply->errorString());
    } else {
        // A payload size of -1 means the sender does not know how much it is going to send,
        // and a sender can also announce less than it ends up sending, so only a file that
        // stops short of the announced size is an incomplete one.
        if (m_size >= 0 && m_written < m_size) {
            qCDebug(KDECONNECT_CORE) << "Received incomplete file (" << m_written << "/" << m_size << "bytes ), deleting";

            deleteDestinationFile();

            setError(3);
            setErrorText(i18n("Received incomplete file from: %1", m_from));
        } else {
            if (m_size >= 0 && m_written > m_size) {
                qCInfo(KDECONNECT_CORE) << "Received" << (m_written - m_size) << "bytes more than the announced" << m_size
                                        << "and keeping them, the sender announced a size it did not keep to";
            }
            qCDebug(KDECONNECT_CORE) << "Finished transfer" << m_destination;
        }
    }
    verifyChecksum();
}

bool FileTransferJob::isSafeDestination() const
{
    if (m_destinationRoot.isEmpty()) {
        return true;
    }
    const QString path = QDir::cleanPath(m_destination.toLocalFile());
    if (path != m_destinationRoot && !path.startsWith(m_destinationRoot + QLatin1Char('/'))) {
        return false;
    }
    QString current = path;
    while (true) {
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) {
            return false;
        }
        if (current == m_destinationRoot) {
            return info.isDir();
        }
        current = info.absolutePath();
    }
}

void FileTransferJob::verifyChecksum()
{
    if (isFinished()) {
        return;
    }
    if (error() || m_expectedChecksum.isEmpty()) {
        emitResult();
        return;
    }
    const QString path = m_destination.toLocalFile();
    connect(&m_checksumWatcher, &QFutureWatcher<QByteArray>::finished, this, [this] {
        if (isFinished()) {
            return;
        }
        if (m_checksumWatcher.result() != m_expectedChecksum) {
            deleteDestinationFile();
            setError(UserDefinedError);
            setErrorText(i18n("SHA-256 verification failed: %1", m_destination.toLocalFile()));
        }
        emitResult();
    });
    m_checksumWatcher.setFuture(QtConcurrent::run([path] {
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
            return QByteArray();
        }
        return hash.result().toHex();
    }));
}

void FileTransferJob::deleteDestinationFile()
{
    if (!m_directory && isSafeDestination() && m_destination.isLocalFile() && QFile::exists(m_destination.toLocalFile())) {
        QFile::remove(m_destination.toLocalFile());
    }
}

bool FileTransferJob::doKill()
{
    if (m_reply) {
        m_reply->close();
    }
    if (m_origin) {
        m_origin->close();
    }

    deleteDestinationFile();

    return true;
}

#include "moc_filetransferjob.cpp"
