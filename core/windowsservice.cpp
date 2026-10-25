/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "windowsservice.h"
#include "daemon/windowsservice/servicestate.h"

#include <windows.h>

#include <sddl.h>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
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

bool WindowsService::removalInProgressForCurrentUser()
{
    const auto config = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(QStringLiteral("kdeconnect"));
    const auto path =
        QDir(config).filePath(QString::fromWCharArray(ServiceState::directoryName) + QLatin1Char('/') + QString::fromWCharArray(ServiceState::fileName));
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        ServiceState::Header header;
        DWORD bytes = 0;
        LARGE_INTEGER size{};
        const bool read = GetFileSizeEx(file, &size) && size.QuadPart >= static_cast<LONGLONG>(ServiceState::minimumSize)
            && size.QuadPart <= static_cast<LONGLONG>(ServiceState::maximumSize) && ReadFile(file, &header, sizeof(header), &bytes, nullptr)
            && bytes == sizeof(header);
        CloseHandle(file);
        return !read || ServiceState::blocksStartup(header);
    }
    const DWORD error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
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
