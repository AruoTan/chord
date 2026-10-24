/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "desktopintegration.h"

#include <windows.h>

#include <propsys.h>

#include <propkey.h>
#include <shlobj.h>

#include <memory>
#include <string_view>
#include <system_error>

namespace
{
void checkResult(HRESULT result)
{
    if (FAILED(result)) {
        throw std::system_error(result, std::system_category(), "Read legacy notification shortcut");
    }
}

struct ComCloser {
    template<typename T>
    void operator()(T *pointer) const
    {
        if (pointer) {
            pointer->Release();
        }
    }
};
}

bool WindowsDesktop::isLegacyNotificationShortcut(const std::filesystem::path &path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        throw std::system_error(error, std::system_category(), "Locate legacy notification shortcut");
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (initialized != RPC_E_CHANGED_MODE) {
        checkResult(initialized);
    }
    struct Apartment {
        bool owned;
        ~Apartment()
        {
            if (owned) {
                CoUninitialize();
            }
        }
    } apartment{SUCCEEDED(initialized)};
    IShellLinkW *rawLink = nullptr;
    checkResult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&rawLink)));
    std::unique_ptr<IShellLinkW, ComCloser> link(rawLink);
    IPersistFile *rawFile = nullptr;
    checkResult(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&rawFile)));
    std::unique_ptr<IPersistFile, ComCloser> file(rawFile);
    if (FAILED(file->Load(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE))) {
        return false;
    }
    IPropertyStore *rawProperties = nullptr;
    checkResult(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void **>(&rawProperties)));
    std::unique_ptr<IPropertyStore, ComCloser> properties(rawProperties);
    struct Property {
        PROPVARIANT value{};
        ~Property()
        {
            PropVariantClear(&value);
        }
    } property;
    checkResult(properties->GetValue(PKEY_AppUserModel_ID, &property.value));
    return property.value.vt == VT_LPWSTR && property.value.pwszVal && std::wstring_view(property.value.pwszVal) == appId;
}

bool WindowsDesktop::removeLegacyNotificationShortcut(const std::filesystem::path &path)
{
    if (!isLegacyNotificationShortcut(path)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        const DWORD writable = attributes & ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(path.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL)) {
            throw std::system_error(GetLastError(), std::system_category(), "Update legacy notification shortcut attributes");
        }
    }
    if (!DeleteFileW(path.c_str())) {
        throw std::system_error(GetLastError(), std::system_category(), "Remove legacy notification shortcut");
    }
    return true;
}
