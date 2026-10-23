/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "core/compositefiletransferjob.h"
#include "transfernotificationtracker.h"

#include <KIO/JobTracker>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamWriter>
#include <chrono>
#include <memory>

#include <notificationactivationcallback.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <windows.h>
#include <wrl.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>

namespace
{
using namespace winrt::Windows::UI::Notifications;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

constexpr wchar_t appId[] = L"org.kde.kdeconnect.transfers";
constexpr wchar_t group[] = L"receiving";
constexpr wchar_t activatorId[] = L"{5FA5D3F1-182B-49FD-A313-C06C5336CC70}";

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
    {
        try {
            initialize();
        } catch (const winrt::hresult_error &error) {
            disable(error);
        }
    }

    ~WindowsTransferNotifications()
    {
        if (m_cookie) {
            CoRevokeClassObject(m_cookie);
        }
        m_notifier = nullptr;
        if (m_uninitialize) {
            RoUninitialize();
        }
    }

    void notify(const TransferNotification &notification, bool replace)
    {
        if (m_failed) {
            return;
        }
        try {
            if (m_notifier.Setting() != NotificationSetting::Enabled) {
                return;
            }
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
                // A dismissed notification stays dismissed until the final result.
                if (m_notifier.Update(data, tag, group) == NotificationUpdateResult::Failed) {
                    qWarning() << "Windows transfer notification update failed";
                }
                return;
            }

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
            toast.Group(group);
            toast.Data(data);
            toast.ExpirationTime(winrt::clock::now() + std::chrono::hours(24));
            toast.Failed([](const ToastNotification &, const ToastFailedEventArgs &args) {
                qWarning() << "Windows transfer notification rejected:" << Qt::hex << static_cast<quint32>(args.ErrorCode().value);
            });
            m_notifier.Show(toast);
        } catch (const winrt::hresult_error &error) {
            disable(error);
        }
    }

private:
    void disable(const winrt::hresult_error &error)
    {
        m_failed = true;
        qWarning() << "Windows transfer notifications unavailable:" << QString::fromWCharArray(error.message().c_str()) << Qt::hex
                   << static_cast<quint32>(error.code().value);
    }

    void initialize()
    {
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
        winrt::check_hresult(CoRegisterClassObject(clsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &m_cookie));

        const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString programs = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
        if (programs.isEmpty() || !QDir().mkpath(programs)) {
            winrt::throw_hresult(E_ACCESSDENIED);
        }
        ComPtr<IShellLinkW> shortcut;
        winrt::check_hresult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)));
        winrt::check_hresult(shortcut->SetPath(executable.toStdWString().c_str()));
        winrt::check_hresult(shortcut->SetArguments(L"--background"));
        winrt::check_hresult(shortcut->SetDescription(L"KDE Connect file transfer notifications"));
        winrt::check_hresult(shortcut->SetIconLocation(executable.toStdWString().c_str(), 0));
        ComPtr<IPropertyStore> properties;
        winrt::check_hresult(shortcut.As(&properties));
        PROPVARIANT value;
        winrt::check_hresult(InitPropVariantFromString(appId, &value));
        const HRESULT appIdResult = properties->SetValue(PKEY_AppUserModel_ID, value);
        PropVariantClear(&value);
        winrt::check_hresult(appIdResult);
        winrt::check_hresult(InitPropVariantFromCLSID(clsid, &value));
        const HRESULT activatorResult = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, value);
        PropVariantClear(&value);
        winrt::check_hresult(activatorResult);
        winrt::check_hresult(properties->Commit());
        ComPtr<IPersistFile> file;
        winrt::check_hresult(shortcut.As(&file));
        const QString link = QDir::toNativeSeparators(programs + QStringLiteral("/KDE Connect Transfers.lnk"));
        winrt::check_hresult(file->Save(link.toStdWString().c_str(), TRUE));

        QSettings server(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\CLSID\\") + QString::fromWCharArray(activatorId)
                             + QStringLiteral("\\LocalServer32"),
                         QSettings::NativeFormat);
        server.setValue(QStringLiteral("."), QStringLiteral("\"%1\" --background").arg(executable));
        server.sync();
        if (server.status() != QSettings::NoError) {
            winrt::throw_hresult(E_ACCESSDENIED);
        }
        m_notifier = ToastNotificationManager::CreateToastNotifier(appId);
    }

    ToastNotifier m_notifier{nullptr};
    DWORD m_cookie = 0;
    bool m_uninitialize = false;
    bool m_failed = false;
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
            notifications->notify(notification, replace);
        },
        parent);
}
