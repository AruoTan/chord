/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "shareplugin.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFutureWatcher>
#include <QMimeData>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <KApplicationTrader>
#include <KFileUtils>
#include <KIO/Job>
#include <KIO/MkpathJob>
#include <KIO/OpenUrlJob>
#include <KJobTrackerInterface>
#include <KLocalizedString>
#include <KNotification>
#include <KPluginFactory>
#include <KSystemClipboard>

#include "core/daemon.h"
#include "core/filetransferjob.h"
#include "core/notificationserverinfo.h"
#include "plugin_share_debug.h"

K_PLUGIN_CLASS_WITH_JSON(SharePlugin, "kdeconnect_share.json")

namespace
{
constexpr int maxFolderEntries = 1000000;

bool validPathComponent(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return false;
    }
    for (const QChar ch : name) {
        if (ch.unicode() < 32 || QStringLiteral("/\\:<>\"|?*").contains(ch)) {
            return false;
        }
    }
    const QString stem = name.section(QLatin1Char('.'), 0, 0).toUpper();
    return stem != QLatin1String("CON") && stem != QLatin1String("PRN") && stem != QLatin1String("AUX") && stem != QLatin1String("NUL")
        && !(stem.size() == 4 && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT"))) && stem.at(3) >= QLatin1Char('1')
             && stem.at(3) <= QLatin1Char('9'));
}

bool collectFolder(const QFileInfo &info, QList<QFileInfo> &entries, QString &error, int depth = 0)
{
    if (!validPathComponent(info.fileName()) || info.isSymLink() || info.isJunction() || !info.isReadable() || (!info.isFile() && !info.isDir()) || depth > 256
        || entries.size() >= maxFolderEntries) {
        error = i18n("Cannot transfer this folder entry: %1", info.absoluteFilePath());
        return false;
    }
    entries.append(info);
    if (info.isDir()) {
        const auto children =
            QDir(info.absoluteFilePath()).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo &child : children) {
            if (!collectFolder(child, entries, error, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

struct ShareEntry {
    QString source;
    NetworkPacket packet;
};

struct PreparedShare {
    QList<ShareEntry> entries;
    QString error;
};

PreparedShare prepareEntries(const QStringList &urls, bool open)
{
    PreparedShare result;
    for (const QString &input : urls) {
        const QUrl url(input);
        if (!url.isLocalFile() || url.isRelative()) {
            NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
            packet.set(QStringLiteral("url"), url.toString());
            result.entries.append({{}, packet});
            continue;
        }
        const QFileInfo root(QDir::cleanPath(url.toLocalFile()));
        const bool folder = root.isDir();
        QList<QFileInfo> files;
        if (folder) {
            if (!collectFolder(root, files, result.error)) {
                return result;
            }
            QSet<QString> paths;
            const QDir dir(root.absoluteFilePath());
            for (const QFileInfo &info : std::as_const(files)) {
                const QString path = dir.relativeFilePath(info.absoluteFilePath()).toCaseFolded();
                if (paths.contains(path)) {
                    result.error = i18n("Folder contains names that differ only in case: %1", info.absoluteFilePath());
                    return result;
                }
                paths.insert(path);
            }
        } else {
            files.append(root);
        }
        const QString folderId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QDir rootDir(root.absoluteFilePath());
        for (qsizetype index = 0; index < files.size(); ++index) {
            const QFileInfo &info = files.at(index);
            NetworkPacket packet(folder ? PACKET_TYPE_SHARE_REQUEST_FOLDER : PACKET_TYPE_SHARE_REQUEST);
            packet.set(QStringLiteral("filename"), info.fileName());
            packet.set(QStringLiteral("creationTime"), info.birthTime().toMSecsSinceEpoch());
            packet.set(QStringLiteral("lastModified"), info.lastModified().toMSecsSinceEpoch());
            packet.set(QStringLiteral("open"), open);
            if (folder) {
                packet.set(QStringLiteral("folderId"), folderId);
                packet.set(QStringLiteral("folderName"), root.fileName());
                packet.set(QStringLiteral("relativePath"), index == 0 ? QString() : rootDir.relativeFilePath(info.absoluteFilePath()));
                packet.set(QStringLiteral("directory"), info.isDir());
                packet.set(QStringLiteral("entryIndex"), index);
                packet.set(QStringLiteral("entryCount"), files.size());
            }
            if (!info.isDir()) {
                QFile file(info.absoluteFilePath());
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!info.isFile() || !file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
                    result.error = i18n("Could not read source file: %1", info.absoluteFilePath());
                    return result;
                }
                packet.set(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
                packet.setPayload(nullptr, file.size());
            }
            result.entries.append({info.absoluteFilePath(), packet});
        }
    }
    return result;
}
}

struct SharePlugin::IncomingFolder {
    QString name;
    QString path;
    int entryCount;
    int nextEntry = 0;
    int pendingJobs = 0;
    bool failed = false;
    QSet<QString> paths;
    QSet<QString> directories;
    QPointer<QTimer> timeout;
};

SharePlugin::SharePlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
    , m_compositeJob()
{
}

QUrl SharePlugin::destinationDir() const
{
    const QString defaultDownloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QUrl dir = QUrl::fromLocalFile(config()->getString(QStringLiteral("incoming_path"), defaultDownloadPath));

    if (dir.path().contains(QLatin1String("%1"))) {
        dir.setPath(dir.path().arg(device()->name()));
    }

    KJob *job = KIO::mkpath(dir);
    bool ret = job->exec();
    if (!ret) {
        qWarning() << "couldn't create" << dir;
    }

    return dir;
}

QUrl SharePlugin::getFileDestination(const QString filename) const
{
    const QDir destDir(destinationDir().toLocalFile());
    const QString dir = QDir::cleanPath(destDir.absolutePath());
    const QString filePath = QDir::cleanPath(destDir.absoluteFilePath(filename));
    if (!filePath.startsWith(dir + QStringLiteral("/"))) {
        qCWarning(KDECONNECT_PLUGIN_SHARE) << "Directory traversal detected";
        return QUrl();
    }
    return QUrl::fromLocalFile(filePath);
}

static QString cleanFilename(const QString &filename)
{
    int idx = filename.lastIndexOf(QLatin1Char('/'));
    return idx >= 0 ? filename.mid(idx + 1) : filename;
}

void SharePlugin::setDateModified(const QUrl &destination, const qint64 timestamp)
{
    if (timestamp == 0) {
        return;
    }
    QFile receivedFile(destination.toLocalFile());
    if (!receivedFile.exists() || !receivedFile.open(QIODevice::ReadWrite)) {
        return;
    }
    receivedFile.setFileTime(QDateTime::fromMSecsSinceEpoch(timestamp), QFileDevice::FileTime(QFileDevice::FileModificationTime));
}

void SharePlugin::setDateCreated(const QUrl &destination, const qint64 timestamp)
{
    if (timestamp == 0) {
        return;
    }
    QFile receivedFile(destination.toLocalFile());
    if (!receivedFile.exists() || !receivedFile.open(QIODevice::ReadWrite)) {
        return;
    }
    receivedFile.setFileTime(QDateTime::fromMSecsSinceEpoch(timestamp), QFileDevice::FileTime(QFileDevice::FileBirthTime));
}

static bool isWebUrl(QUrl &url)
{
    if (!url.isValid()) {
        return false;
    }
    const QString scheme = url.scheme();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https") || scheme == QStringLiteral("mailto");
}

void SharePlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_SHARE_REQUEST_FOLDER) {
        receiveFolderEntry(np);
        return;
    }

    qCDebug(KDECONNECT_PLUGIN_SHARE) << "File transfer";

    if (np.hasPayload() || np.has(QStringLiteral("filename"))) {
        //         qCDebug(KDECONNECT_PLUGIN_SHARE) << "receiving file" << filename << "in" << dir << "into" << destination;
        const QString filename = cleanFilename(np.get<QString>(QStringLiteral("filename"), QString::number(QDateTime::currentMSecsSinceEpoch())));
        const QUrl destination = getFileDestination(filename);
        if (destination.isEmpty()) {
            return;
        }
        const qint64 dateCreated = np.get<qint64>(QStringLiteral("creationTime"), QDateTime::currentMSecsSinceEpoch());
        const qint64 dateModified = np.get<qint64>(QStringLiteral("lastModified"), QDateTime::currentMSecsSinceEpoch());
        const bool open = np.get<bool>(QStringLiteral("open"), false);

        FileTransferJob *job = np.createPayloadTransferJob(destination);
        job->setOriginName(device()->name() + QStringLiteral(": ") + filename);
        job->setAutoRenameIfDestinationExists(true);
        connect(job, &KJob::result, this, [this, dateCreated, dateModified, open](KJob *job) -> void {
            finished(job, dateCreated, dateModified, open);
        });
        addTransferJob(job);
    } else if (np.has(QStringLiteral("text"))) {
        QString text = np.get<QString>(QStringLiteral("text"));

        auto mimeData = new QMimeData;
        mimeData->setText(text);
        KSystemClipboard::instance()->setMimeData(mimeData, QClipboard::Clipboard);

        QUrl url;
        QStringList lines = text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
        if (!lines.isEmpty()) {
            url.setUrl(lines.last().trimmed());
        }

        KNotification *notif = new KNotification(QStringLiteral("textShareReceived"));
        notif->setComponentName(QStringLiteral("kdeconnect"));
        notif->setText(text);
        if (NotificationServerInfo::instance().supportedHints().testFlag(NotificationServerInfo::X_KDE_ORIGIN_NAME)) {
            notif->setTitle(i18nc("@info Some piece of text was received from a connected device", "Shared text copied to clipboard", device()->name()));
            notif->setHint(QStringLiteral("x-kde-origin-name"), device()->name());
        } else {
            notif->setTitle(
                i18nc("@info Some piece of text was received from a connected device", "Shared text from %1 copied to clipboard", device()->name()));
        }

        // Mark this notification so sendnotifications can filter it out and not echo it back to the device
        notif->setHint(QStringLiteral("x-kdeconnect-source-device"), device()->name());

        auto openTextEditor = [this, text, notif] {
            KService::Ptr service = KApplicationTrader::preferredService(QStringLiteral("text/plain"));
            const QString defaultApp = service ? service->desktopEntryName() : QString();

            if (defaultApp == QLatin1String("org.kde.kate") || defaultApp == QLatin1String("org.kde.kwrite")) {
                QProcess *proc = new QProcess();
#ifdef Q_OS_UNIX
                proc->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
                connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
                if (const QString token = notif->xdgActivationToken(); !token.isEmpty()) {
                    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                    env.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), token);
                    proc->setProcessEnvironment(env);
                }
                proc->start(defaultApp.section(QStringLiteral("."), 2, 2), QStringList(QStringLiteral("--stdin")));
                proc->write(text.toUtf8());
                proc->closeWriteChannel();
            } else {
                QTemporaryFile tmpFile;
                QString tmpFilename = QStringLiteral("kdeconnect-XXXXXX.txt");
                tmpFile.setFileTemplate(tmpFilename);
                tmpFile.setAutoRemove(false);
                if (!tmpFile.open()) {
                    qWarning() << "Couldn't open temp file:" << tmpFilename;
                    return;
                }
                tmpFile.write(text.toUtf8());
                tmpFile.close();

                const QString fileName = tmpFile.fileName();
                auto *job = new KIO::OpenUrlJob(QUrl::fromLocalFile(fileName), QStringLiteral("text/plain"));
                job->setStartupId(notif->xdgActivationToken().toUtf8());
                job->start();
                Q_EMIT shareReceived(fileName);
            }
        };

        KNotificationAction *textEditorAction = notif->addAction(i18nc("@action:button Edit text with default text editor", "Open in Text Editor"));
        connect(textEditorAction, &KNotificationAction::activated, this, openTextEditor);

        if (isWebUrl(url)) {
            KNotificationAction *openLinkAction = notif->addAction(i18nc("@action:button Open URL with default app", "Open Link"));
            connect(openLinkAction, &KNotificationAction::activated, this, [this, url, notif] {
                auto *job = new KIO::OpenUrlJob(url);
                job->setStartupId(notif->xdgActivationToken().toUtf8());
                job->start();
                Q_EMIT shareReceived(url.toString());
            });
        }
        notif->sendEvent();

    } else if (np.has(QStringLiteral("url"))) {
        QUrl url = QUrl::fromEncoded(np.get<QByteArray>(QStringLiteral("url")));
        if (isWebUrl(url)) {
            QDesktopServices::openUrl(url);
        }
        Q_EMIT shareReceived(url.toString());
    } else {
        qCDebug(KDECONNECT_PLUGIN_SHARE) << "Error: Nothing attached!";
    }
}

void SharePlugin::addTransferJob(FileTransferJob *job)
{
    if (!m_compositeJob || !m_compositeJob->isRunning()) {
        m_compositeJob = new CompositeFileTransferJob(device(), this);
        m_compositeJob->setProperty("destUrl", destinationDir().toString());
        m_compositeJob->setProperty("immediateProgressReporting", true);
        connect(m_compositeJob, &KJob::finished, this, [this](KJob *job) {
            if (job->error()) {
                for (const auto &folder : std::as_const(m_incomingFolders)) {
                    folder->failed = true;
                    if (folder->timeout) {
                        folder->timeout->deleteLater();
                    }
                }
                m_incomingFolders.clear();
            }
        });
        Daemon::instance()->jobTracker()->registerJob(m_compositeJob);
    }
    m_compositeJob->addSubjob(job);
    if (!m_compositeJob->isRunning()) {
        m_compositeJob->start();
    }
}

void SharePlugin::receiveFolderEntry(const NetworkPacket &np)
{
    const QString id = np.get<QString>(QStringLiteral("folderId"));
    const QString name = np.get<QString>(QStringLiteral("folderName"));
    const QString relative = np.get<QString>(QStringLiteral("relativePath"));
    const bool directory = np.get<bool>(QStringLiteral("directory"));
    const int index = np.get<int>(QStringLiteral("entryIndex"), -1);
    const int count = np.get<int>(QStringLiteral("entryCount"));
    auto folder = m_incomingFolders.value(id);
    const auto reject = [this, &np, &folder, &id] {
        if (np.payload()) {
            np.payload()->close();
        }
        if (folder) {
            folder->failed = true;
            if (folder->timeout) {
                folder->timeout->deleteLater();
            }
            m_incomingFolders.remove(id);
        }
        Daemon::instance()->reportError(i18n("Could not receive folder"), i18n("Invalid or unsafe folder transfer from %1", device()->name()));
    };

    if (QUuid(id).isNull() || !validPathComponent(name) || count < 1 || count > maxFolderEntries || index < 0 || index >= count
        || (directory && np.hasPayload()) || (!directory && (np.payloadSize() < 0 || np.get<QString>(QStringLiteral("sha256")).size() != 64))) {
        reject();
        return;
    }
    const QStringList parts = relative.split(QLatin1Char('/'));
    if (parts.size() > 256) {
        reject();
        return;
    }
    if (!relative.isEmpty()) {
        for (const QString &part : parts) {
            if (!validPathComponent(part)) {
                reject();
                return;
            }
        }
    }
    if (!folder) {
        if (index != 0 || !relative.isEmpty() || !directory) {
            reject();
            return;
        }
        const QDir destination(destinationDir().toLocalFile());
        QString rootName = name;
        if (QFileInfo::exists(destination.filePath(rootName))) {
            rootName = KFileUtils::suggestName(QUrl::fromLocalFile(destination.path()), rootName);
        }
        if (!destination.mkdir(rootName)) {
            reject();
            return;
        }
        folder = QSharedPointer<IncomingFolder>::create();
        folder->name = name;
        folder->path = QDir::cleanPath(destination.absoluteFilePath(rootName));
        folder->entryCount = count;
        folder->timeout = new QTimer(this);
        folder->timeout->setSingleShot(true);
        folder->timeout->setInterval(60000);
        connect(folder->timeout, &QTimer::timeout, this, [this, id, folder] {
            folder->failed = true;
            m_incomingFolders.remove(id);
            folder->timeout->deleteLater();
            Daemon::instance()->reportError(i18n("Could not receive folder"), i18n("Folder transfer from %1 was interrupted", device()->name()));
        });
        m_incomingFolders.insert(id, folder);
    }
    const QString key = relative.toCaseFolded();
    const QString parent = parts.size() > 1 ? parts.mid(0, parts.size() - 1).join(QLatin1Char('/')).toCaseFolded() : QString();
    if (folder->failed || folder->nextEntry != index || folder->entryCount != count || folder->name != name || folder->paths.contains(key)
        || (index > 0 && (relative.isEmpty() || !folder->directories.contains(parent)))) {
        reject();
        return;
    }
    folder->paths.insert(key);
    if (directory) {
        folder->directories.insert(key);
    }
    ++folder->nextEntry;
    ++folder->pendingJobs;
    folder->timeout->stop();
    const QUrl destination = QUrl::fromLocalFile(relative.isEmpty() ? folder->path : folder->path + QLatin1Char('/') + relative);
    FileTransferJob *job = np.createPayloadTransferJob(destination);
    job->setDirectoryTransfer(folder->path, directory);
    job->setOriginName(device()->name() + QStringLiteral(": ") + name + QLatin1Char('/') + relative);
    const qint64 created = np.get<qint64>(QStringLiteral("creationTime"));
    const qint64 modified = np.get<qint64>(QStringLiteral("lastModified"));
    connect(job, &KJob::finished, this, [this, folder, id, directory, created, modified](KJob *job) {
        --folder->pendingJobs;
        folder->failed |= job->error() != 0;
        if (!directory && !job->error()) {
            finished(job, created, modified, false);
        }
        if (folder->failed || (folder->nextEntry == folder->entryCount && folder->pendingJobs == 0)) {
            m_incomingFolders.remove(id);
            if (folder->timeout) {
                folder->timeout->deleteLater();
            }
            if (!folder->failed) {
                Q_EMIT shareReceived(QUrl::fromLocalFile(folder->path).toString());
            }
        } else if (folder->pendingJobs == 0 && folder->timeout) {
            folder->timeout->start();
        }
    });
    addTransferJob(job);
}

void SharePlugin::finished(KJob *job, const qint64 dateCreated, const qint64 dateModified, const bool open)
{
    FileTransferJob *ftjob = qobject_cast<FileTransferJob *>(job);
    if (ftjob && !job->error()) {
        setDateCreated(ftjob->destination(), dateCreated);
        setDateModified(ftjob->destination(), dateModified);
        Q_EMIT shareReceived(ftjob->destination().toString());
        qCDebug(KDECONNECT_PLUGIN_SHARE) << "File transfer finished." << ftjob->destination();
        if (open) {
            QDesktopServices::openUrl(ftjob->destination());
        }
    } else {
        qCDebug(KDECONNECT_PLUGIN_SHARE) << "File transfer failed." << (ftjob ? ftjob->destination() : QUrl());
    }
}

void SharePlugin::openDestinationFolder()
{
    QDesktopServices::openUrl(destinationDir());
}

void SharePlugin::shareUrl(const QUrl &url, bool open)
{
    prepareShare({url.toString()}, open);
}

void SharePlugin::shareUrls(const QStringList &urls)
{
    prepareShare(urls, false);
}

void SharePlugin::prepareShare(const QStringList &urls, bool open)
{
    for (const QString &input : urls) {
        const QUrl url(input);
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir() && !device()->supportsPacketType(PACKET_TYPE_SHARE_REQUEST_FOLDER)) {
            Daemon::instance()->reportError(i18n("Could not share folder"),
                                            i18n("%1 does not support folder transfers. Update KDE Connect on that device.", device()->name()));
            return;
        }
    }
    auto *watcher = new QFutureWatcher<PreparedShare>(this);
    connect(watcher, &QFutureWatcher<PreparedShare>::finished, this, [this, watcher] {
        const PreparedShare prepared = watcher->result();
        watcher->deleteLater();
        if (!prepared.error.isEmpty()) {
            Daemon::instance()->reportError(i18n("Could not share files"), prepared.error);
            return;
        }
        for (const ShareEntry &entry : prepared.entries) {
            NetworkPacket packet = entry.packet;
            if (!entry.source.isEmpty()) {
                if (packet.get<bool>(QStringLiteral("directory"))) {
                    packet.setPayload(QSharedPointer<QBuffer>::create(), 0);
                } else {
                    packet.setPayload(QSharedPointer<QFile>::create(entry.source), packet.payloadSize());
                }
            }
            if (!sendPacket(packet)) {
                Daemon::instance()->reportError(i18n("Could not share files"), i18n("Failed to send to %1", device()->name()));
                return;
            }
        }
    });
    watcher->setFuture(QtConcurrent::run(prepareEntries, urls, open));
}

void SharePlugin::shareText(const QString &text)
{
    NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
    packet.set<QString>(QStringLiteral("text"), text);
    sendPacket(packet);
}

QString SharePlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/share").arg(device()->id());
}

#include "moc_shareplugin.cpp"
#include "shareplugin.moc"
