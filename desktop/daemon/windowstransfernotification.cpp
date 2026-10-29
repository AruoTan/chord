/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "core/compositefiletransferjob.h"
#include "core/kdeconnectconfig.h"
#include "core/windowsservice.h"
#include "transfernotificationtracker.h"
#include "windowsservice/desktopintegration.h"

#include <KIO/JobTracker>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QXmlStreamWriter>
#include <chrono>
#include <memory>

#include <notificationactivationcallback.h>
#include <shobjidl.h>
#include <windows.h>
#include <wrl.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>

namespace
{
using namespace winrt::Windows::UI::Notifications;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

using WindowsDesktop::activatorId;
using WindowsDesktop::appId;

void logNotification(const QString &message)
{
    static const QString path = KdeConnectConfig::instance().baseConfigDir().filePath(QStringLiteral("transfer-notifications.log"));
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    QFile file(path);
    if (file.size() > 1024 * 1024) {
        QFile::remove(path + QStringLiteral(".1"));
        file.rename(path + QStringLiteral(".1"));
        file.setFileName(path);
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(QStringLiteral("%1 pid=%2 %3\n")
                       .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                       .arg(QCoreApplication::applicationPid())
                       .arg(message)
                       .toUtf8());
    }
    qInfo().noquote() << "Windows transfer notifications:" << message;
}

QString settingName(NotificationSetting setting)
{
    switch (setting) {
    case NotificationSetting::Enabled:
        return QStringLiteral("Enabled");
    case NotificationSetting::DisabledForApplication:
        return QStringLiteral("DisabledForApplication");
    case NotificationSetting::DisabledForUser:
        return QStringLiteral("DisabledForUser");
    case NotificationSetting::DisabledByGroupPolicy:
        return QStringLiteral("DisabledByGroupPolicy");
    case NotificationSetting::DisabledByManifest:
        return QStringLiteral("DisabledByManifest");
    }
    return QString::number(static_cast<int>(setting));
}

class NotificationActivator : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, INotificationActivationCallback>
{
public:
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR, LPCWSTR, const NOTIFICATION_USER_INPUT_DATA *, ULONG) override
    {
        // Transfer notifications do not open the app or indicator.
        return S_OK;
    }
};

class NotificationFactory : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory>
{
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **result) override
    {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        auto activator = Make<NotificationActivator>();
        return activator ? activator.CopyTo(iid, result) : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override
    {
        return S_OK;
    }
};

winrt::hstring hstring(const QString &text)
{
    return winrt::hstring(text.toStdWString());
}

class WindowsTransferNotifications
{
public:
    WindowsTransferNotifications()
        : m_group(WindowsService::registeredForCurrentUser() ? WindowsDesktop::serviceGroup : WindowsDesktop::interactiveGroup)
    {
        logNotification(QStringLiteral("Starting notification backend: %1").arg(QCoreApplication::applicationFilePath()));
        ensureReady();
    }

    ~WindowsTransferNotifications()
    {
        reset();
    }

    bool notify(const TransferNotification &notification, bool replace)
    {
        if ((m_retryTimer.isValid() && !m_retryTimer.hasExpired(5000)) || !ensureReady()) {
            return false;
        }
        try {
            m_stage = QStringLiteral("Read notification settings");
            if (!notificationsEnabled()) {
                return false;
            }
            m_stage = QStringLiteral("Build notification data");
            NotificationData data;
            data.SequenceNumber(notification.sequence);
            data.Values().Insert(L"title", hstring(notification.title));
            data.Values().Insert(L"text", hstring(notification.text.left(2048)));
            data.Values().Insert(L"status", hstring(notification.status));
            data.Values().Insert(L"progressText", hstring(notification.progressText));
            data.Values().Insert(L"progress",
                                 hstring(notification.progress < 0 ? QStringLiteral("indeterminate") : QString::number(notification.progress, 'f', 4)));
            const auto tag = hstring(notification.tag);
            if (!replace) {
                m_stage = QStringLiteral("Update notification");
                // A dismissed notification stays dismissed until the final result.
                if (m_notifier.Update(data, tag, m_group) == NotificationUpdateResult::Failed) {
                    logNotification(QStringLiteral("Update failed: tag=%1").arg(notification.tag));
                    m_retryTimer.start();
                    return false;
                }
                m_retryTimer.invalidate();
                return true;
            }

            m_stage = QStringLiteral("Build notification XML");
            QString xml;
            QXmlStreamWriter writer(&xml);
            writer.writeStartElement(QStringLiteral("toast"));
            writer.writeStartElement(QStringLiteral("visual"));
            writer.writeStartElement(QStringLiteral("binding"));
            writer.writeAttribute(QStringLiteral("template"), QStringLiteral("ToastGeneric"));
            writer.writeTextElement(QStringLiteral("text"), QStringLiteral("{title}"));
            writer.writeTextElement(QStringLiteral("text"), QStringLiteral("{text}"));
            if (!notification.finished) {
                writer.writeStartElement(QStringLiteral("progress"));
                writer.writeAttribute(QStringLiteral("value"), QStringLiteral("{progress}"));
                writer.writeAttribute(QStringLiteral("valueStringOverride"), QStringLiteral("{progressText}"));
                writer.writeAttribute(QStringLiteral("status"), QStringLiteral("{status}"));
                writer.writeEndElement();
            }
            writer.writeEndElement();
            writer.writeEndElement();
            writer.writeEndElement();
            winrt::Windows::Data::Xml::Dom::XmlDocument document;
            document.LoadXml(hstring(xml));
            ToastNotification toast(document);
            toast.Tag(tag);
            toast.Group(m_group);
            toast.Data(data);
            toast.ExpirationTime(winrt::clock::now() + std::chrono::hours(24));
            toast.Failed([tag = notification.tag](const ToastNotification &, const ToastFailedEventArgs &args) {
                logNotification(QStringLiteral("Notification rejected: tag=%1 HRESULT=0x%2")
                                    .arg(tag)
                                    .arg(static_cast<quint32>(args.ErrorCode().value), 8, 16, QLatin1Char('0')));
            });
            m_stage = QStringLiteral("Show notification");
            m_lastToast = toast;
            m_notifier.Show(toast);
            logNotification(
                QStringLiteral("Show accepted: tag=%1 phase=%2").arg(notification.tag, notification.finished ? notification.title : notification.status));
            m_retryTimer.invalidate();
            return true;
        } catch (const winrt::hresult_error &error) {
            reportFailure(error);
            return false;
        }
    }

private:
    bool notificationsEnabled()
    {
        if (m_settingsUnavailable) {
            return true;
        }
        try {
            const auto setting = m_notifier.Setting();
            if (m_lastSetting != static_cast<int>(setting)) {
                logNotification(QStringLiteral("Notification setting: %1").arg(settingName(setting)));
                m_lastSetting = static_cast<int>(setting);
            }
            return setting == NotificationSetting::Enabled;
        } catch (const winrt::hresult_error &error) {
            if (error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
                throw;
            }
            // Setting can be unavailable for unpackaged desktop apps; Show still enforces Windows preferences.
            logNotification(QStringLiteral("Notification settings unavailable: HRESULT=0x80070490; attempting Show/Update"));
            m_settingsUnavailable = true;
            return true;
        }
    }

    void reportFailure(const winrt::hresult_error &error)
    {
        logNotification(QStringLiteral("%1 failed: HRESULT=0x%2 %3")
                            .arg(m_stage)
                            .arg(static_cast<quint32>(error.code().value), 8, 16, QLatin1Char('0'))
                            .arg(QString::fromWCharArray(error.message().c_str())));
        m_retryTimer.start();
    }

    void reset()
    {
        m_lastToast = nullptr;
        m_notifier = nullptr;
        if (m_cookie) {
            CoRevokeClassObject(m_cookie);
            m_cookie = 0;
        }
        if (m_uninitialize) {
            RoUninitialize();
            m_uninitialize = false;
        }
        m_lastSetting = -1;
        m_settingsUnavailable = false;
    }

    bool ensureReady()
    {
        if (m_notifier) {
            return true;
        }
        try {
            initialize();
            m_retryTimer.invalidate();
            logNotification(QStringLiteral("Notification identity registered: org.kde.kdeconnect.transfers"));
            return true;
        } catch (const winrt::hresult_error &error) {
            reportFailure(error);
            reset();
            return false;
        }
    }

    void initialize()
    {
        m_stage = QStringLiteral("RoInitialize");
        const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
        m_uninitialize = SUCCEEDED(initialized);
        if (initialized != RPC_E_CHANGED_MODE) {
            winrt::check_hresult(initialized);
        }
        GUID clsid;
        winrt::check_hresult(CLSIDFromString(activatorId, &clsid));
        auto factory = Make<NotificationFactory>();
        if (!factory) {
            winrt::throw_hresult(E_OUTOFMEMORY);
        }
        m_stage = QStringLiteral("Register COM class factory");
        winrt::check_hresult(CoRegisterClassObject(clsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &m_cookie));

        const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        m_stage = QStringLiteral("Register COM server");
        QSettings server(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\CLSID\\") + QString::fromWCharArray(activatorId)
                             + QStringLiteral("\\LocalServer32"),
                         QSettings::NativeFormat);
        server.setValue(QStringLiteral("."), QStringLiteral("\"%1\" --background").arg(executable));
        server.sync();
        if (server.status() != QSettings::NoError) {
            winrt::throw_hresult(E_ACCESSDENIED);
        }
        m_stage = QStringLiteral("Register AUMID");
        QSettings identity(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\AppUserModelId\\") + QString::fromWCharArray(appId), QSettings::NativeFormat);
        identity.setValue(QStringLiteral("DisplayName"), QStringLiteral("KDE Connect Transfers"));
        identity.setValue(QStringLiteral("CustomActivator"), QString::fromWCharArray(activatorId));
        identity.sync();
        if (identity.status() != QSettings::NoError) {
            winrt::throw_hresult(E_ACCESSDENIED);
        }
        winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(appId));
        m_stage = QStringLiteral("CreateToastNotifier");
        m_notifier = ToastNotificationManager::CreateToastNotifier(appId);
    }

    ToastNotifier m_notifier{nullptr};
    ToastNotification m_lastToast{nullptr};
    winrt::hstring m_group;
    DWORD m_cookie = 0;
    bool m_uninitialize = false;
    int m_lastSetting = -1;
    bool m_settingsUnavailable = false;
    QString m_stage;
    QElapsedTimer m_retryTimer;
};

class WindowsTransferTracker : public TransferNotificationTracker
{
public:
    using TransferNotificationTracker::TransferNotificationTracker;

    void registerJob(KJob *job) override
    {
        if (qobject_cast<CompositeFileTransferJob *>(job)) {
            TransferNotificationTracker::registerJob(job);
        } else {
            KIO::getJobTracker()->registerJob(job);
        }
    }
};
}

KJobTrackerInterface *createWindowsTransferTracker(QObject *parent)
{
    auto notifications = std::make_shared<WindowsTransferNotifications>();
    return new WindowsTransferTracker(
        [notifications](const TransferNotification &notification, bool replace) {
            return notifications->notify(notification, replace);
        },
        parent);
}
