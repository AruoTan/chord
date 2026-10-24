/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <windows.h>

#include <propsys.h>

#include <propkey.h>
#include <propvarutil.h>
#include <sddl.h>
#include <shlobj.h>

#include "desktopintegration.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && std::wstring(argv[1]) == L"--remove-legacy-shortcut") {
        WindowsDesktop::removeLegacyNotificationShortcut(argv[2]);
        return 0;
    }
    if (argc == 4 && std::wstring(argv[1]) == L"--create-shortcut") {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) {
            return 10;
        }
        IShellLinkW *link = nullptr;
        HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&link));
        if (SUCCEEDED(result)) {
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, 32768);
            result = link->SetPath(executable);
            IPropertyStore *properties = nullptr;
            if (SUCCEEDED(result)) {
                result = link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void **>(&properties));
            }
            if (SUCCEEDED(result)) {
                PROPVARIANT value{};
                result = InitPropVariantFromString(argv[3], &value);
                if (SUCCEEDED(result)) {
                    result = properties->SetValue(PKEY_AppUserModel_ID, value);
                }
                PropVariantClear(&value);
                if (SUCCEEDED(result)) {
                    result = properties->Commit();
                }
                properties->Release();
            }
            IPersistFile *file = nullptr;
            if (SUCCEEDED(result)) {
                result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file));
            }
            if (SUCCEEDED(result)) {
                result = file->Save(argv[2], TRUE);
                file->Release();
            }
            link->Release();
        }
        CoUninitialize();
        return SUCCEEDED(result) ? 0 : 11;
    }
    std::wstring heartbeatName;
    std::wstring stopName;
    bool background = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument(argv[index]);
        if (argument == L"--background") {
            background = true;
        } else if ((argument == L"--service-heartbeat" || argument == L"--service-stop") && index + 1 < argc) {
            (argument == L"--service-heartbeat" ? heartbeatName : stopName) = argv[++index];
        }
    }
    HANDLE heartbeat = OpenEventW(EVENT_MODIFY_STATE, FALSE, heartbeatName.c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
    if (!background || !heartbeat || !stop) {
        return 1;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return 2;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size)) {
        CloseHandle(token);
        return 3;
    }
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) {
        return 4;
    }
    const std::wstring wideSid(sid);
    LocalFree(sid);
    const std::string userSid(wideSid.begin(), wideSid.end());
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    const auto directory = std::filesystem::path(buffer).parent_path();
    {
        std::ofstream status(directory / L"worker.json.new");
        status << "{\"pid\":" << GetCurrentProcessId() << ",\"session\":" << session << ",\"sid\":\"" << userSid << "\",\"background\":true}";
    }
    MoveFileExW((directory / L"worker.json.new").c_str(), (directory / L"worker.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    for (;;) {
        if (std::filesystem::exists(directory / L"pause-heartbeat")) {
            // Simulate a blocked event loop that cannot acknowledge a graceful stop.
            Sleep(200);
            continue;
        }
        if (WaitForSingleObject(stop, 200) != WAIT_TIMEOUT) {
            break;
        }
        SetEvent(heartbeat);
    }
    std::ofstream stopped(directory / L"worker-stopped.txt", std::ios::app);
    stopped << GetCurrentProcessId() << '\n';
    CloseHandle(stop);
    CloseHandle(heartbeat);
    return 0;
}
