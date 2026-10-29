/**
 * SPDX-FileCopyrightText: 2026 Albert Vaca Cintora <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>

#include "core/backends/lan/lanlinkprovider.h"

class TestLanRateLimit : public QObject
{
    Q_OBJECT
public:
    TestLanRateLimit()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

private Q_SLOTS:
    void testTargetedDiscovery()
    {
        const QHostAddress target(QStringLiteral("127.0.0.2"));
        QUdpSocket receiver;
        QVERIFY(receiver.bind(target, LanLinkProvider::UDP_PORT));
        LanLinkProvider provider(true);
        provider.onStart();
        provider.discoverAddresses({target});
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        QByteArray packet(receiver.pendingDatagramSize(), '\0');
        QVERIFY(receiver.readDatagram(packet.data(), packet.size()) > 0);
        QCOMPARE(QJsonDocument::fromJson(packet).object().value(QStringLiteral("type")).toString(), QStringLiteral("kdeconnect.identity"));

        provider.disable();
        while (receiver.hasPendingDatagrams()) {
            receiver.readDatagram(packet.data(), packet.size());
        }
        provider.discoverAddresses({target});
        QTest::qWait(50);
        QVERIFY(!receiver.hasPendingDatagrams());
    }

    void testRateLimitTcpByIp()
    {
        LanLinkProvider provider(true);
        provider.onStart();
        QVERIFY(provider.tcpPort() != 0);

        // Let the rate-limit entry created by receiving our own UDP broadcast expire
        QTest::qWait(600);

        QTcpSocket first;
        QVERIFY(connectAndCheckAccepted(first, provider.tcpPort()));

        QTcpSocket second;
        QVERIFY(!connectAndCheckAccepted(second, provider.tcpPort()));

        QTest::qWait(600);

        QTcpSocket third;
        QVERIFY(connectAndCheckAccepted(third, provider.tcpPort()));

        provider.onStop();
    }

private:
    // Returns false if the server drops the connection right away
    bool connectAndCheckAccepted(QTcpSocket &socket, quint16 port)
    {
        QSignalSpy disconnectedSpy(&socket, &QAbstractSocket::disconnected);
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!socket.waitForConnected(1000)) {
            return false;
        }
        // The server waits 1s for an identity packet before closing an accepted connection
        return !disconnectedSpy.wait(300);
    }
};

QTEST_MAIN(TestLanRateLimit);

#include "lanratelimittest.moc"
