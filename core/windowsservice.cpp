/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "windowsservice.h"

#include <windows.h>

#include <sddl.h>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QTextStream>

bool WindowsService::registeredForCurrentUser()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray data(size, '\0');
    const bool valid = GetTokenInformation(token, TokenUser, data.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!valid || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) {
        return false;
    }
    const QString path = QStringLiteral("SYSTEM\\CurrentControlSet\\Services\\KDEConnectBackground-%1\\Parameters").arg(QString::fromWCharArray(sid));
    LocalFree(sid);
    DWORD type = 0;
    DWORD bytes = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, reinterpret_cast<LPCWSTR>(path.utf16()), L"DaemonPath", RRF_RT_REG_SZ, &type, nullptr, &bytes) == ERROR_SUCCESS;
}

int WindowsService::configure(bool install)
{
    QProcess helper;
    helper.setProgram(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("kdeconnect-service.exe")));
    helper.setArguments({install ? QStringLiteral("--register") : QStringLiteral("--unregister")});
    helper.setProcessChannelMode(QProcess::ForwardedChannels);
    helper.start();
    if (!helper.waitForStarted(5000)) {
        QTextStream(stderr) << "Cannot start the bundled Windows service helper: " << helper.errorString() << Qt::endl;
        return 1;
    }
    if (!helper.waitForFinished(150000)) {
        helper.kill();
        helper.waitForFinished();
        QTextStream(stderr) << "Windows service configuration timed out" << Qt::endl;
        return 1;
    }
    return helper.exitStatus() == QProcess::NormalExit ? helper.exitCode() : 1;
}
