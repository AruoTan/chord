// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#ifndef IPADDRESSVALIDATOR_H
#define IPADDRESSVALIDATOR_H

#include <QHostAddress>
#include <QQmlEngine>
#include <QValidator>

class IpAddressValidator : public QValidator
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit IpAddressValidator(QObject *parent = nullptr)
        : QValidator(parent)
    {
    }

    Q_INVOKABLE QString normalizedAddress(const QString &input) const
    {
        QHostAddress address(input.trimmed());
        bool isIPv4 = false;
        const quint32 ipv4 = address.toIPv4Address(&isIPv4);
        if (isIPv4) {
            address = QHostAddress(ipv4);
        }
        if (address.isNull() || address.isMulticast() || address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6
            || address == QHostAddress::Broadcast) {
            return {};
        }
        return address.toString();
    }

    State validate(QString &input, int &pos) const override
    {
        Q_UNUSED(pos)
        return normalizedAddress(input).isEmpty() ? Intermediate : Acceptable;
    }

    void fixup(QString &input) const override
    {
        input = normalizedAddress(input);
    }
};

#endif
