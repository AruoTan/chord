/**
 * SPDX-FileCopyrightText: 2015 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <backends/lan/uploadjob.h>
#include <core/compositefiletransferjob.h>
#include <core/filetransferjob.h>
#include <core/pluginloader.h>
#include <kdeconnectconfig.h>

#include "core/daemon.h"
#include "core/device.h"
#include "core/kdeconnectplugin.h"
#include "kdeconnect-version.h"
#include "testdaemon.h"
#include "testdevice.h"
#include <backends/lan/compositeuploadjob.h>
#include <backends/lan/lanlinkprovider.h>
#include <backends/pairinghandler.h>
#include <plugins/share/shareplugin.h>

class TestSendFile : public QObject
{
    Q_OBJECT
public:
    TestSendFile()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_daemon = new TestDaemon;
    }

private Q_SLOTS:
    void testCompositeVerificationStatus()
    {
        TestDevice device(this, KdeConnectConfig::instance().deviceId());
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const QByteArray content("verified file");
        auto input = QSharedPointer<QBuffer>::create();
        input->setData(content);
        QVERIFY(input->open(QIODevice::ReadOnly));
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
        packet.setPayload(input, content.size());
        packet.set(QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()));
        packet.set(QStringLiteral("totalPayloadSize"), content.size());
        packet.set(QStringLiteral("numberOfFiles"), 1);
        CompositeFileTransferJob composite(&device, this);
        composite.setAutoDelete(false);
        QSignalSpy status(&composite, &KJob::infoMessage);
        QSignalSpy result(&composite, &KJob::result);
        connect(&composite, &KJob::infoMessage, this, [&composite, &content, &result](KJob *job, const QString &) {
            QCOMPARE(job, &composite);
            QCOMPARE(job->processedAmount(KJob::Bytes), qulonglong(content.size()));
            QCOMPARE(result.size(), 0);
        });
        QVERIFY(composite.addSubjob(packet.createPayloadTransferJob(QUrl::fromLocalFile(destination.filePath(QStringLiteral("received.txt"))))));
        composite.start();
        QVERIFY(result.wait());
        QCOMPARE(composite.error(), 0);
        QCOMPARE(status.size(), 1);
        QCOMPARE(status.first().at(1).toString(), QStringLiteral("Verifying SHA-256..."));
    }

    void testChecksum_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<bool>("valid");
        QTest::newRow("valid") << QByteArray("file contents") << true;
        QTest::newRow("same-size-corruption") << QByteArray("file contents") << false;
        QTest::newRow("valid-empty") << QByteArray() << true;
        QTest::newRow("corrupt-empty") << QByteArray() << false;
    }

    void testChecksum()
    {
        QFETCH(QByteArray, content);
        QFETCH(bool, valid);
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const QString path = destination.filePath(QStringLiteral("received.txt"));
        auto input = QSharedPointer<QBuffer>::create();
        input->setData(content);
        QVERIFY(input->open(QIODevice::ReadOnly));
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
        packet.setPayload(input, content.size());
        const QByteArray expected = valid ? content : QByteArray(content.size(), 'x');
        const QByteArray digest = QCryptographicHash::hash(valid || !content.isEmpty() ? expected : QByteArray("x"), QCryptographicHash::Sha256).toHex();
        packet.set(QStringLiteral("sha256"), QString::fromLatin1(digest));
        auto *job = packet.createPayloadTransferJob(QUrl::fromLocalFile(path));
        job->setAutoDelete(false);
        const auto cleanup = qScopeGuard([job] {
            delete job;
        });
        QSignalSpy result(job, &KJob::result);
        QSignalSpy status(job, &KJob::infoMessage);
        job->start();
        QVERIFY(result.wait());
        QCOMPARE(status.size(), 1);
        QCOMPARE(status.first().at(1).toString(), QStringLiteral("Verifying SHA-256..."));
        QCOMPARE(job->error() == 0, valid);
        QCOMPARE(QFileInfo::exists(path), valid);
        if (valid) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), content);
        }
    }

    void testMissingPayload_data()
    {
        QTest::addColumn<bool>("stream");
        QTest::newRow("missing-payload") << false;
        QTest::newRow("closed-empty-stream") << true;
    }

    void testMissingPayload()
    {
        QFETCH(bool, stream);
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        auto input = QSharedPointer<QBuffer>::create();
        QVERIFY(input->open(QIODevice::ReadOnly));
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
        packet.setPayload(stream ? input : QSharedPointer<QIODevice>(), 5);
        const QString path = destination.filePath(QStringLiteral("missing.txt"));
        auto *job = packet.createPayloadTransferJob(QUrl::fromLocalFile(path));
        job->setAutoDelete(false);
        const auto cleanup = qScopeGuard([job] {
            delete job;
        });
        QSignalSpy result(job, &KJob::result);
        job->start();
        if (stream) {
            QMetaObject::invokeMethod(input.data(), "readChannelFinished", Qt::QueuedConnection);
        }
        QVERIFY(result.wait());
        QVERIFY(job->error());
        QVERIFY(!QFileInfo::exists(path));
    }

    void testSendFolders()
    {
        Device *device = m_daemon->getDevice(KdeConnectConfig::instance().deviceId());
        QVERIFY(device);
        if (!device->isPaired()) {
            device->requestPairing();
        }
        QVERIFY(device->supportsPacketType(PACKET_TYPE_SHARE_REQUEST_FOLDER));
        KdeConnectPlugin *plugin = device->plugin(QStringLiteral("kdeconnect_share"));
        QVERIFY(plugin);
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        const QString previous =
            plugin->config()->getString(QStringLiteral("incoming_path"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        const auto restore = qScopeGuard([plugin, previous] {
            plugin->config()->set(QStringLiteral("incoming_path"), previous);
        });
        plugin->config()->set(QStringLiteral("incoming_path"), destination.path());

        const QMap<QString, QByteArray> files = {
            {QStringLiteral("folderA/sub/file.txt"), QByteArray("first folder")},
            {QStringLiteral("folderA/sub/empty.txt"), QByteArray()},
            {QStringLiteral("folderA/.hidden"), QByteArray("hidden file")},
            {QStringLiteral("folderB/sub/file.txt"), QByteArray("second folder")},
            {QStringLiteral("loose.txt"), QByteArray("loose file")},
        };
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const QString path = source.filePath(it.key());
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(it.value()), it.value().size());
        }
        QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("folderA/emptyDir"))));
        QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("emptyFolder"))));
        QStringList urls;
        for (const QString &name : {QStringLiteral("folderA"), QStringLiteral("folderB"), QStringLiteral("emptyFolder"), QStringLiteral("loose.txt")}) {
            urls.append(QUrl::fromLocalFile(source.filePath(name)).toString());
        }
        QSignalSpy received(plugin, SIGNAL(shareReceived(QString)));
        QVERIFY(plugin->metaObject()->invokeMethod(plugin, "shareUrls", Q_ARG(QStringList, urls)));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), files.size() + 3, 10000);
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QFile file(destination.filePath(it.key()));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            QCOMPARE(file.readAll(), it.value());
        }
        QVERIFY(QFileInfo(destination.filePath(QStringLiteral("folderA/emptyDir"))).isDir());
        QVERIFY(QFileInfo(destination.filePath(QStringLiteral("emptyFolder"))).isDir());

        received.clear();
        QVERIFY(plugin->metaObject()->invokeMethod(plugin, "shareUrl", Q_ARG(QString, urls.first())));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 4, 10000);
        const QUrl rootUrl(received.last().first().toString());
        QVERIFY(rootUrl.toLocalFile() != destination.filePath(QStringLiteral("folderA")));
        QFile duplicate(QDir(rootUrl.toLocalFile()).filePath(QStringLiteral("sub/file.txt")));
        QVERIFY(duplicate.open(QIODevice::ReadOnly));
        QCOMPARE(duplicate.readAll(), QByteArray("first folder"));
    }

    void testLegacyPeer()
    {
        DeviceInfo info = KdeConnectConfig::instance().deviceInfo();
        info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        info.incomingCapabilities.remove(PACKET_TYPE_SHARE_REQUEST_FOLDER);
        TestDevice peer(this, info.id);
        peer.updateDeviceInfo(info);
        KdeConnectPlugin *plugin = PluginLoader::instance()->instantiatePluginForDevice(QStringLiteral("kdeconnect_share"), &peer);
        QVERIFY(plugin);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(".*does not support folder transfers.*")));
        QVERIFY(plugin->metaObject()->invokeMethod(plugin, "shareUrl", Q_ARG(QString, QUrl::fromLocalFile(folder.path()).toString())));
        QCOMPARE(peer.getSentPackets(), 0);

        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(file.write("contents"), 8);
        file.close();
        QVERIFY(plugin->metaObject()->invokeMethod(plugin, "shareUrl", Q_ARG(QString, QUrl::fromLocalFile(file.fileName()).toString())));
        QTRY_COMPARE(peer.getSentPackets(), 1);
        QCOMPARE(peer.getLastPacket()->type(), QString(PACKET_TYPE_SHARE_REQUEST));
        QCOMPARE(peer.getLastPacket()->payloadSize(), 8);
        QCOMPARE(peer.getLastPacket()->get<QString>(QStringLiteral("sha256")),
                 QString::fromLatin1(QCryptographicHash::hash("contents", QCryptographicHash::Sha256).toHex()));
    }

    void testUnsafeFolderPath_data()
    {
        QTest::addColumn<QString>("relative");
        QTest::newRow("parent") << QStringLiteral("../outside.txt");
        QTest::newRow("absolute") << QStringLiteral("/outside.txt");
        QTest::newRow("drive") << QStringLiteral("C:/outside.txt");
        QTest::newRow("backslash") << QStringLiteral("sub\\outside.txt");
        QTest::newRow("alternate-stream") << QStringLiteral("file:stream");
        QTest::newRow("reserved") << QStringLiteral("NUL.txt");
    }

    void testFolderOverTls()
    {
        Device *receiver = m_daemon->getDevice(KdeConnectConfig::instance().deviceId());
        QVERIFY(receiver);
        if (!receiver->isPaired()) {
            receiver->requestPairing();
        }
        KdeConnectPlugin *plugin = receiver->plugin(QStringLiteral("kdeconnect_share"));
        QVERIFY(plugin);
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const QString previous =
            plugin->config()->getString(QStringLiteral("incoming_path"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        const auto restore = qScopeGuard([plugin, previous] {
            plugin->config()->set(QStringLiteral("incoming_path"), previous);
        });
        plugin->config()->set(QStringLiteral("incoming_path"), destination.path());
        TestDevice sender(this, receiver->id());
        const auto connection = connect(&sender, &TestDevice::packetSent, this, [this, plugin, receiver](const NetworkPacket &header) {
            if (header.type() != PACKET_TYPE_SHARE_REQUEST_FOLDER) {
                return;
            }
            NetworkPacket packet = header;
            if (packet.hasPayload()) {
                auto socket = QSharedPointer<QSslSocket>::create();
                LanLinkProvider::configureSslSocket(socket.data(), receiver->id(), true);
                connect(socket.data(), &QAbstractSocket::disconnected, socket.data(), &QAbstractSocket::readChannelFinished);
                socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), header.payloadTransferInfo().value(QStringLiteral("port")).toUInt());
                packet.setPayload(socket, packet.payloadSize());
            }
            plugin->receivePacket(packet);
        });
        const auto disconnectSender = qScopeGuard([connection] {
            QObject::disconnect(connection);
        });
        auto *upload = new CompositeUploadJob(&sender, false);
        upload->setAutoDelete(false);
        const auto cleanup = qScopeGuard([upload] {
            delete upload;
        });
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QStringList paths = {QString(), QStringLiteral("sub"), QStringLiteral("sub/data.bin"), QStringLiteral("sub/zero.txt"), QStringLiteral("empty")};
        const QByteArray content(512 * 1024, 'x');
        QTemporaryFile source;
        QVERIFY(source.open());
        QCOMPARE(source.write(content), content.size());
        source.close();
        for (int index = 0; index < paths.size(); ++index) {
            const bool directory = index == 0 || index == 1 || index == 4;
            NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST_FOLDER);
            packet.set(QStringLiteral("folderId"), id);
            packet.set(QStringLiteral("folderName"), QStringLiteral("folder"));
            packet.set(QStringLiteral("relativePath"), paths.at(index));
            packet.set(QStringLiteral("filename"), paths.at(index).section(QLatin1Char('/'), -1));
            packet.set(QStringLiteral("directory"), directory);
            packet.set(QStringLiteral("entryIndex"), index);
            packet.set(QStringLiteral("entryCount"), paths.size());
            if (index == 2) {
                packet.setPayload(QSharedPointer<QFile>::create(source.fileName()), content.size());
            } else {
                packet.setPayload(QSharedPointer<QBuffer>::create(), 0);
            }
            if (!directory) {
                packet.set(QStringLiteral("sha256"),
                           QString::fromLatin1(QCryptographicHash::hash(index == 2 ? content : QByteArray(), QCryptographicHash::Sha256).toHex()));
            }
            upload->addSubjob(new UploadJob(packet));
        }
        QSignalSpy received(plugin, SIGNAL(shareReceived(QString)));
        QSignalSpy sent(upload, &KJob::result);
        upload->start();
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 3, 10000);
        QTRY_COMPARE(sent.count(), 1);
        QCOMPARE(upload->error(), 0);
        QFile file(destination.filePath(QStringLiteral("folder/sub/data.bin")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
        QVERIFY(QFileInfo(destination.filePath(QStringLiteral("folder/sub/zero.txt"))).isFile());
        QVERIFY(QFileInfo(destination.filePath(QStringLiteral("folder/empty"))).isDir());
    }

    void testFolderSymlinkDestination()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir root;
        QTemporaryDir outside;
        QVERIFY(root.isValid());
        QVERIFY(outside.isValid());
        QVERIFY(QFile::link(outside.path(), root.filePath(QStringLiteral("link"))));
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST_FOLDER);
        auto *job = packet.createPayloadTransferJob(QUrl::fromLocalFile(root.filePath(QStringLiteral("link/outside.txt"))));
        job->setDirectoryTransfer(root.path(), false);
        job->setAutoDelete(false);
        const auto cleanup = qScopeGuard([job] {
            delete job;
        });
        QSignalSpy result(job, &KJob::result);
        job->start();
        QVERIFY(result.wait());
        QVERIFY(job->error());
        QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("outside.txt"))));
#else
        QSKIP("This test requires Unix symbolic links");
#endif
    }

    void testUnsafeFolderPath()
    {
        QFETCH(QString, relative);
        Device *device = m_daemon->getDevice(KdeConnectConfig::instance().deviceId());
        QVERIFY(device);
        if (!device->isPaired()) {
            device->requestPairing();
        }
        KdeConnectPlugin *plugin = device->plugin(QStringLiteral("kdeconnect_share"));
        QVERIFY(plugin);
        QTemporaryDir destination;
        const QString previous =
            plugin->config()->getString(QStringLiteral("incoming_path"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        const auto restore = qScopeGuard([plugin, previous] {
            plugin->config()->set(QStringLiteral("incoming_path"), previous);
        });
        plugin->config()->set(QStringLiteral("incoming_path"), destination.path());
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST_FOLDER);
        packet.set(QStringLiteral("folderId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        packet.set(QStringLiteral("folderName"), QStringLiteral("root"));
        packet.set(QStringLiteral("directory"), true);
        packet.set(QStringLiteral("entryIndex"), 0);
        packet.set(QStringLiteral("entryCount"), 2);
        plugin->receivePacket(packet);
        packet.set(QStringLiteral("relativePath"), relative);
        packet.set(QStringLiteral("directory"), false);
        packet.set(QStringLiteral("entryIndex"), 1);
        packet.set(QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash({}, QCryptographicHash::Sha256).toHex()));
        plugin->receivePacket(packet);
        QCoreApplication::processEvents();
        QCOMPARE(QDir(destination.filePath(QStringLiteral("root"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
    }

    void testSend()
    {
        if (!(m_daemon->getLinkProviders().size() > 0)) {
            QFAIL("No links available, but loopback should have been provided by the test");
        }

        const auto deviceIds = m_daemon->devices();
        Device *device = nullptr;
        for (const QString &deviceId : deviceIds) {
            Device *d = m_daemon->getDevice(deviceId);
            if (d->isReachable()) {
                if (!d->isPaired())
                    d->requestPairing();
                device = d;
            }
        }
        if (device == nullptr) {
            QFAIL("Unable to determine device");
        }
        QCOMPARE(device->isReachable(), true);
        QCOMPARE(device->isPaired(), true);

        QByteArray content("12312312312313213123213123");

        QTemporaryFile temp;
        QVERIFY(temp.open());
        temp.write(content);
        temp.close();

        KdeConnectPlugin *plugin = device->plugin(QStringLiteral("kdeconnect_share"));
        QVERIFY(plugin);
        plugin->metaObject()->invokeMethod(plugin, "shareUrl", Q_ARG(QString, QUrl::fromLocalFile(temp.fileName()).toString()));

        QSignalSpy spy(plugin, SIGNAL(shareReceived(QString)));
        QVERIFY(spy.wait(2000));

        QVariantList args = spy.takeFirst();
        QUrl sentFile(args.first().toUrl());

        QFile file(sentFile.toLocalFile());
        QCOMPARE(file.size(), content.size());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
    }

    void testSendEmptyFile()
    {
        // An empty file has no payload, but it should still be sent, counted and go through the
        // same composite job machinery as any other file (fixes empty files being dropped from the
        // total count, or transfers stalling/crashing when an empty file is mixed in with others).
        if (!(m_daemon->getLinkProviders().size() > 0)) {
            QFAIL("No links available, but loopback should have been provided by the test");
        }

        const auto deviceIds = m_daemon->devices();
        Device *device = nullptr;
        for (const QString &deviceId : deviceIds) {
            Device *d = m_daemon->getDevice(deviceId);
            if (d->isReachable()) {
                if (!d->isPaired())
                    d->requestPairing();
                device = d;
            }
        }
        if (device == nullptr) {
            QFAIL("Unable to determine device");
        }
        QCOMPARE(device->isReachable(), true);
        QCOMPARE(device->isPaired(), true);

        QTemporaryFile temp;
        QVERIFY(temp.open());
        temp.close();
        QCOMPARE(QFileInfo(temp.fileName()).size(), 0);

        KdeConnectPlugin *plugin = device->plugin(QStringLiteral("kdeconnect_share"));
        QVERIFY(plugin);
        plugin->metaObject()->invokeMethod(plugin, "shareUrl", Q_ARG(QString, QUrl::fromLocalFile(temp.fileName()).toString()));

        QSignalSpy spy(plugin, SIGNAL(shareReceived(QString)));
        QVERIFY(spy.wait(2000));

        QVariantList args = spy.takeFirst();
        QUrl sentFile(args.first().toUrl());

        QFile file(sentFile.toLocalFile());
        QVERIFY(file.exists());
        QCOMPARE(file.size(), 0);
    }

    void testSslJobs()
    {
        const QString aFile = QFINDTESTDATA("sendfiletest.cpp");
        const QString destFile = QDir::tempPath() + QStringLiteral("/kdeconnect-test-sentfile");
        QFile(destFile).remove();

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        Device *device = new Device(this, deviceInfo.id);
        m_daemon->addDevice(device);

        QSharedPointer<QFile> f(new QFile(aFile));
        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.setPayload(f, f->size());

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        UploadJob *uj = new UploadJob(np);
        job->addSubjob(uj);

        QSignalSpy spyUpload(job, &KJob::result);
        job->start();

        QVERIFY(f->open(QIODevice::ReadWrite));

        FileTransferJob *ft = np.createPayloadTransferJob(QUrl::fromLocalFile(destFile));

        QSignalSpy spyTransfer(ft, &KJob::result);

        ft->start();

        QVERIFY(spyTransfer.count() || spyTransfer.wait());

        if (ft->error()) {
            qWarning() << "fterror" << ft->errorString();
        }

        QCOMPARE(ft->error(), 0);

        QCOMPARE(spyUpload.count(), 1);

        QFile resultFile(destFile), originFile(aFile);
        QVERIFY(resultFile.open(QIODevice::ReadOnly));
        QVERIFY(originFile.open(QIODevice::ReadOnly));

        const QByteArray resultContents = resultFile.readAll(), originContents = originFile.readAll();
        QCOMPARE(resultContents.size(), originContents.size());
        QCOMPARE(resultFile.readAll(), originFile.readAll());
    }

    void testUploadEmptyFile()
    {
        // A packet with no payload set at all (like the ones sent for empty files) must still
        // flow through CompositeUploadJob/UploadJob as a normal, immediately-completing subjob,
        // so it gets counted instead of being silently dropped from the total.
        const QString destFile = QDir::tempPath() + QStringLiteral("/kdeconnect-test-empty-upload");
        QFile(destFile).remove();

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        Device *device = new Device(this, deviceInfo.id);
        m_daemon->addDevice(device);

        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.set<QString>(QStringLiteral("filename"), QStringLiteral("empty.txt"));
        QVERIFY(!np.hasPayload());

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        UploadJob *uj = new UploadJob(np);
        job->addSubjob(uj);

        // Like testSslJobs(), sending the packet over the (fake) device link is not actually
        // exercised here; we only check that the no-payload subjob completes on its own instead
        // of hanging forever waiting for a socket connection that will never come.
        QSignalSpy spyUpload(job, &KJob::result);
        job->start();

        QVERIFY(spyUpload.count() || spyUpload.wait());
        // Once finished (however it finished), the job must not be mistaken for still-usable by
        // whoever holds onto it (e.g. LanDeviceLink), or a later file would be silently attached
        // to this already-finished job and never actually get sent.
        QVERIFY(!job->isRunning());

        FileTransferJob *ft = np.createPayloadTransferJob(QUrl::fromLocalFile(destFile));
        QSignalSpy spyTransfer(ft, &KJob::result);
        ft->start();

        QVERIFY(spyTransfer.count() || spyTransfer.wait());
        QCOMPARE(ft->error(), 0);

        QFile resultFile(destFile);
        QVERIFY(resultFile.exists());
        QCOMPARE(resultFile.size(), 0);
    }

    void testUploadMissingInput()
    {
        QTemporaryFile temporaryFile;
        QVERIFY(temporaryFile.open());
        const QString missingFilePath = temporaryFile.fileName();
        temporaryFile.close();
        QVERIFY(QFile::remove(missingFilePath));

        QSharedPointer<QFile> input(new QFile(missingFilePath));
        NetworkPacket packet(PACKET_TYPE_SHARE_REQUEST);
        packet.setPayload(input, 1);

        UploadJob job(packet);
        QSignalSpy resultSpy(&job, &KJob::result);
        job.start();

        QVERIFY(resultSpy.wait());
        QVERIFY(job.error());
        QVERIFY(!job.errorText().isEmpty());
    }

    void testMultipleEmptyFilesInOneCompositeUpload()
    {
        // Regression test for a bug where sending several empty files in one go got the count
        // wrong and the transfer stuck: for each file, CompositeUploadJob sends a per-file
        // "header" packet to announce the transfer, and that packet must go straight out over
        // the wire rather than be mistaken for yet another new file to enqueue. For files with a
        // payload, the header's payload device was explicitly stripped before sending; for files
        // with no payload (like empty files, which still carry a valid-but-empty QIODevice per
        // SharePlugin::shareUrl()) it wasn't - so LanDeviceLink treated it as a new file, feeding
        // it back into the same composite job and leaving it stuck waiting for a subjob that
        // would never actually arrive (see bug report: 3 empty files shown as "1 of 4", stuck).
        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        TestDevice *device = new TestDevice(this, deviceInfo.id);

        QTemporaryFile empty;
        QVERIFY(empty.open());
        empty.close();

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        for (int i = 0; i < 3; ++i) {
            QSharedPointer<QFile> f(new QFile(empty.fileName()));
            NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
            np.setPayload(f, 0);
            np.set<QString>(QStringLiteral("filename"), QStringLiteral("empty%1.txt").arg(i));
            QVERIFY(!np.hasPayload());
            job->addSubjob(new UploadJob(np));
        }

        QSignalSpy spyUpload(job, &KJob::result);
        job->start();

        QVERIFY(spyUpload.count() || spyUpload.wait());
        QCOMPARE(job->error(), 0);
        QVERIFY(!job->isRunning());

        // Exactly one header packet per file must have gone out - not fewer (stuck) and not more
        // (a file's own header being mistaken for another new file).
        QCOMPARE(device->getSentPackets(), 3);

        NetworkPacket *last = device->getLastPacket();
        QVERIFY(last);
        QCOMPARE(last->type(), QString(PACKET_TYPE_SHARE_REQUEST));
        QCOMPARE(last->get<int>(QStringLiteral("numberOfFiles")), 3);
        // The header must not carry the payload device onward: it's just an announcement, not
        // something the other end should try to open a transfer socket for.
        QVERIFY(!last->payload());
    }

    void testReceiverConnectsForNextFileEarly()
    {
        // While a file is being sent, the next one is announced and the receiver (e.g. Android)
        // connects for it right away, but only reads from it once it's done with the current file.
        // That early connection must not be mixed up with the current transfer.
        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        TestDevice *device = new TestDevice(this, deviceInfo.id);

        const int numFiles = 3;
        const qint64 size = 64 * 1024 * 1024;
        QList<QSharedPointer<QTemporaryFile>> originFiles;
        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        // The sender can finish before the last receiver does, so don't let the job get deleted while we wait for that one
        job->setAutoDelete(false);
        const auto deleteJob = qScopeGuard([job] {
            delete job;
        });
        for (int i = 0; i < numFiles; ++i) {
            QSharedPointer<QTemporaryFile> originFile(new QTemporaryFile);
            QVERIFY(originFile->open());
            QByteArray block(1024 * 1024, char('a' + i));
            for (qint64 written = 0; written < size; written += block.size()) {
                block[0] = char(written / block.size()); // Make every block different
                QCOMPARE(originFile->write(block), block.size());
            }
            originFile->close();
            originFiles.append(originFile);

            QSharedPointer<QFile> input(new QFile(originFile->fileName()));
            NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
            np.setPayload(input, size);
            job->addSubjob(new UploadJob(np));
        }

        QSignalSpy spyUpload(job, &KJob::result);
        job->start();

        // Connect for every file as soon as it's announced (real clients announce the next file as soon as the first starts its transfer)
        QList<FileTransferJob *> transferJobs;
        QStringList destFiles;
        auto connectForAnnouncedFile = [&]() {
            QTRY_COMPARE_WITH_TIMEOUT(device->getSentPackets(), transferJobs.size() + 1, 10000);
            const quint16 port = device->getLastPacket()->payloadTransferInfo().value(QStringLiteral("port")).toInt();
            QVERIFY(port);

            QSharedPointer<QSslSocket> socket(new QSslSocket);
            LanLinkProvider::configureSslSocket(socket.data(), deviceInfo.id, true);
            connect(socket.data(), &QAbstractSocket::disconnected, socket.data(), &QAbstractSocket::readChannelFinished);
            // Don't buffer the whole file before we start reading it, so the sender is still sending it when we connect for the next one
            socket->setReadBufferSize(64 * 1024);
            socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, QIODevice::ReadWrite);

            const QString destFile = QDir::tempPath() + QStringLiteral("/kdeconnect-test-early-%1").arg(transferJobs.size());
            QFile(destFile).remove();
            NetworkPacket receiveNp(PACKET_TYPE_SHARE_REQUEST);
            receiveNp.setPayload(socket, size);
            transferJobs.append(receiveNp.createPayloadTransferJob(QUrl::fromLocalFile(destFile)));
            destFiles.append(destFile);
        };

        connectForAnnouncedFile();
        for (int i = 0; i < numFiles; ++i) {
            if (i + 1 < numFiles) {
                connectForAnnouncedFile(); // The next file gets announced once this one starts
                if (QTest::currentTestFailed()) {
                    return;
                }
            }
            QSignalSpy spyTransfer(transferJobs[i], &KJob::result);
            transferJobs[i]->start();
            QVERIFY(spyTransfer.wait(30000));
            QCOMPARE(transferJobs[i]->error(), 0);

            QFile resultFile(destFiles[i]);
            QVERIFY(originFiles[i]->open());
            QVERIFY(resultFile.open(QIODevice::ReadOnly));
            QCOMPARE(resultFile.size(), size);
            QCOMPARE(resultFile.readAll(), originFiles[i]->readAll());
            originFiles[i]->close();
            resultFile.remove();
        }

        QVERIFY(spyUpload.count() || spyUpload.wait());
        QCOMPARE(job->error(), 0);
        QCOMPARE(device->getSentPackets(), numFiles);
    }

    void testMoreDataThanAnnounced()
    {
        // A sender can announce a size that turns out to be smaller than what it then sends,
        // for instance when the file it reads is being rewritten as it goes. The file arrives
        // whole, so it has to be kept.
        const QString aFile = QFINDTESTDATA("sendfiletest.cpp");
        const QString destFile = QDir::tempPath() + QStringLiteral("/kdeconnect-test-longer-than-announced");
        QFile(destFile).remove();

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        Device *device = new Device(this, deviceInfo.id);
        m_daemon->addDevice(device);

        QSharedPointer<QFile> f(new QFile(aFile));
        const qint64 actualSize = f->size();
        QVERIFY(actualSize > 16);

        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.setPayload(f, actualSize - 16);

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        UploadJob *uj = new UploadJob(np);
        job->addSubjob(uj);
        job->start();

        QVERIFY(f->open(QIODevice::ReadWrite));

        FileTransferJob *ft = np.createPayloadTransferJob(QUrl::fromLocalFile(destFile));
        QSignalSpy spyTransfer(ft, &KJob::result);
        ft->start();

        QVERIFY(spyTransfer.count() || spyTransfer.wait());

        if (ft->error()) {
            qWarning() << "fterror" << ft->errorString();
        }
        QCOMPARE(ft->error(), 0);

        QFile resultFile(destFile);
        QVERIFY(resultFile.exists());
        QCOMPARE(resultFile.size(), actualSize);
    }

    void testUnannouncedSize()
    {
        // A sender that does not know how big the payload is announces a size of -1, which the
        // Android app does whenever the content provider has no size to give. Nothing was
        // promised, so nothing can fall short of it.
        const QString aFile = QFINDTESTDATA("sendfiletest.cpp");
        const QString destFile = QDir::tempPath() + QStringLiteral("/kdeconnect-test-unannounced-size");
        QFile(destFile).remove();

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        Device *device = new Device(this, deviceInfo.id);
        m_daemon->addDevice(device);

        QSharedPointer<QFile> f(new QFile(aFile));
        const qint64 actualSize = f->size();

        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.setPayload(f, -1);

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
        UploadJob *uj = new UploadJob(np);
        job->addSubjob(uj);
        job->start();

        QVERIFY(f->open(QIODevice::ReadWrite));

        FileTransferJob *ft = np.createPayloadTransferJob(QUrl::fromLocalFile(destFile));
        QSignalSpy spyTransfer(ft, &KJob::result);
        ft->start();

        QVERIFY(spyTransfer.count() || spyTransfer.wait());

        if (ft->error()) {
            qWarning() << "fterror" << ft->errorString();
        }
        QCOMPARE(ft->error(), 0);

        QFile resultFile(destFile);
        QVERIFY(resultFile.exists());
        QCOMPARE(resultFile.size(), actualSize);
    }

    void testTimeout()
    {
        const QString aFile = QFINDTESTDATA("sendfiletest.cpp");

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        TestDevice *device = new TestDevice(this, deviceInfo.id);
        m_daemon->addDevice(device);

        QSharedPointer<QFile> f(new QFile(aFile));
        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.setPayload(f, f->size());

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
#ifdef BUILD_TESTING
        job->testSetTimeoutMs(1000);
#endif
        UploadJob *uj = new UploadJob(np);
        job->addSubjob(uj);

        QSignalSpy spy(job, &KJob::result);
        job->start();

        QVERIFY(spy.wait(5000));
        QCOMPARE(job->error(), CompositeUploadJob::ConnectionTimeoutError);
    }

    void testTlsHandshakeTimeout()
    {
        const QString aFile = QFINDTESTDATA("sendfiletest.cpp");

        DeviceInfo deviceInfo = KdeConnectConfig::instance().deviceInfo();
        KdeConnectConfig::instance().addTrustedDevice(deviceInfo);

        TestDevice *device = new TestDevice(this, deviceInfo.id);
        m_daemon->addDevice(device);

        QSharedPointer<QFile> f(new QFile(aFile));
        NetworkPacket np(PACKET_TYPE_SHARE_REQUEST);
        np.setPayload(f, f->size());

        CompositeUploadJob *job = new CompositeUploadJob(device, false);
#ifdef BUILD_TESTING
        job->testSetTimeoutMs(1000);
#endif
        job->addSubjob(new UploadJob(np));

        QSignalSpy spy(job, &KJob::result);
        job->start();

        QTRY_VERIFY(device->getLastPacket());
        const quint16 port = device->getLastPacket()->payloadTransferInfo().value(QStringLiteral("port")).toUInt();
        QVERIFY(port);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(socket.waitForConnected());

        QVERIFY(spy.wait(5000));
        QCOMPARE(job->error(), CompositeUploadJob::ConnectionTimeoutError);
    }

private:
    TestDaemon *m_daemon;
};

QTEST_MAIN(TestSendFile);

#include "sendfiletest.moc"
