/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>

#include "desktopintegration.h"
#include "servicestate.h"

#ifdef _MSC_VER
#include <roapi.h>
#include <winrt/Windows.UI.Notifications.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace
{
struct HandleCloser {
    void operator()(void *handle) const
    {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
struct ServiceCloser {
    void operator()(void *handle) const
    {
        if (handle) {
            CloseServiceHandle(static_cast<SC_HANDLE>(handle));
        }
    }
};
struct LocalCloser {
    void operator()(void *pointer) const
    {
        LocalFree(pointer);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;
using ServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceCloser>;
using LocalMemory = std::unique_ptr<void, LocalCloser>;

void check(BOOL success, const char *operation)
{
    if (!success) {
        throw std::system_error(GetLastError(), std::system_category(), operation);
    }
}

ULONGLONG awakeMilliseconds()
{
    ULONGLONG time = 0;
    check(QueryUnbiasedInterruptTime(&time), "Read service watchdog clock");
    return time / 10000;
}

std::wstring tokenSid(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    check(GetTokenInformation(token, TokenUser, buffer.data(), size, &size), "GetTokenInformation");
    LPWSTR text = nullptr;
    check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text), "ConvertSidToStringSid");
    LocalMemory memory(text);
    return text;
}

std::wstring currentSid()
{
    HANDLE raw = nullptr;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "OpenProcessToken");
    Handle token(raw);
    return tokenSid(token.get());
}

std::wstring serviceName(const std::wstring &sid)
{
    return L"KDEConnectBackground-" + sid;
}

std::wstring quote(const std::wstring &argument)
{
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else {
            result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
            backslashes = 0;
            if (character == L'"') {
                result += L'\\';
            }
            result += character;
        }
    }
    result.append(backslashes * 2, L'\\');
    return result + L'"';
}

std::filesystem::path executablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    check(size && size < buffer.size(), "GetModuleFileName");
    buffer.resize(size);
    return buffer;
}

LocalMemory securityDescriptor(const std::wstring &sddl)
{
    PSECURITY_DESCRIPTOR raw = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr), "Security descriptor");
    return LocalMemory(raw);
}

void protectPath(const std::filesystem::path &path)
{
    auto descriptor = securityDescriptor(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)");
    PACL acl = nullptr;
    PSID owner = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    check(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted), "Get directory ACL");
    check(GetSecurityDescriptorOwner(descriptor.get(), &owner, &defaulted), "Get directory owner");
    const DWORD error = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
                                              SE_FILE_OBJECT,
                                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                              owner,
                                              nullptr,
                                              acl,
                                              nullptr);
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(), "Protect service path");
    }
}

std::filesystem::path hostDirectory(const std::wstring &sid, bool create)
{
    PWSTR folder = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder);
    if (FAILED(result)) {
        throw std::runtime_error("Cannot locate Program Files");
    }
    std::filesystem::path path(folder);
    CoTaskMemFree(folder);
    for (const auto &part : {std::wstring(L"KDE Connect Background"), sid}) {
        path /= part;
        if (!create) {
            continue;
        }
        auto descriptor = securityDescriptor(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
        if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) {
            check(FALSE, "Create protected service directory");
        }
        const DWORD flags = GetFileAttributesW(path.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Service directory must not be a reparse point");
        }
        protectPath(path);
    }
    return path;
}

std::wstring parameterKey(const std::wstring &sid)
{
    return L"SYSTEM\\CurrentControlSet\\Services\\" + serviceName(sid) + L"\\Parameters";
}

void saveDaemonPath(const std::wstring &sid, const std::wstring &path)
{
    HKEY key = nullptr;
    LSTATUS error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, parameterKey(sid).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error == ERROR_SUCCESS) {
        error = RegSetValueExW(key,
                               L"DaemonPath",
                               0,
                               REG_SZ,
                               reinterpret_cast<const BYTE *>(path.c_str()),
                               static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(), "Save service configuration");
    }
}

std::wstring loadDaemonPath(const std::wstring &sid)
{
    DWORD size = 0;
    LSTATUS error = RegGetValueW(HKEY_LOCAL_MACHINE, parameterKey(sid).c_str(), L"DaemonPath", RRF_RT_REG_SZ, nullptr, nullptr, &size);
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(), "Read service configuration");
    }
    std::wstring path(size / sizeof(wchar_t), L'\0');
    error = RegGetValueW(HKEY_LOCAL_MACHINE, parameterKey(sid).c_str(), L"DaemonPath", RRF_RT_REG_SZ, nullptr, path.data(), &size);
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(), "Read daemon path");
    }
    path.resize(wcslen(path.c_str()));
    if (!std::filesystem::path(path).is_absolute()) {
        throw std::runtime_error("Daemon path must be absolute");
    }
    return path;
}

void waitForState(SC_HANDLE service, DWORD state)
{
    const ULONGLONG deadline = GetTickCount64() + 30000;
    do {
        SERVICE_STATUS_PROCESS status{};
        DWORD size = 0;
        check(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status), &size), "Query service status");
        if (status.dwCurrentState == state) {
            return;
        }
        Sleep(100);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("Timed out waiting for the Windows service");
}

void stopService(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS before{};
    DWORD size = 0;
    check(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&before), sizeof(before), &size), "Query service process");
    if (before.dwCurrentState == SERVICE_STOPPED) {
        return;
    }
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, before.dwProcessId));
    SERVICE_STATUS status{};
    if (before.dwCurrentState != SERVICE_STOP_PENDING && !ControlService(service, SERVICE_CONTROL_STOP, &status)
        && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        check(FALSE, "Stop service");
    }
    waitForState(service, SERVICE_STOPPED);
    if (process && WaitForSingleObject(process.get(), 10000) == WAIT_TIMEOUT) {
        throw std::runtime_error("Service process has not finished stopping");
    }
}

void copyHost(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    check(input.get() != INVALID_HANDLE_VALUE, "Read bundled service host");
    if (!DeleteFileW(destination.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        check(FALSE, "Remove temporary service host");
    }
    auto descriptor = securityDescriptor(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(output.get() != INVALID_HANDLE_VALUE, "Create protected service host");
    BYTE buffer[65536];
    DWORD count = 0;
    for (;;) {
        check(ReadFile(input.get(), buffer, sizeof(buffer), &count, nullptr), "Read service host");
        if (!count) {
            break;
        }
        DWORD written = 0;
        check(WriteFile(output.get(), buffer, count, &written, nullptr) && written == count, "Write service host");
    }
    check(FlushFileBuffers(output.get()), "Flush service host");
}

struct RegistryCloser {
    void operator()(HKEY key) const
    {
        if (key) {
            RegCloseKey(key);
        }
    }
};
using RegistryHandle = std::unique_ptr<std::remove_pointer_t<HKEY>, RegistryCloser>;

void checkRegistry(LSTATUS error, const char *operation)
{
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(), operation);
    }
}

RegistryHandle openKey(HKEY root, const std::wstring &path, bool create = false, REGSAM access = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS)
{
    HKEY key = nullptr;
    const auto error = create ? RegCreateKeyExW(root,
                                                path.c_str(),
                                                0,
                                                nullptr,
                                                0,
                                                KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                                                nullptr,
                                                &key,
                                                nullptr)
                              : RegOpenKeyExW(root, path.c_str(), 0, access, &key);
    if (!create && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
        return {};
    }
    checkRegistry(error, "Open desktop state registry key");
    return RegistryHandle(key);
}

void deleteKey(HKEY root, const std::wstring &path)
{
    auto key = openKey(root, path, false, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (!key) {
        return;
    }
    checkRegistry(RegDeleteTreeW(key.get(), nullptr), "Remove desktop state registry contents");
    key.reset();
    const auto error = RegDeleteKeyW(root, path.c_str());
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        checkRegistry(error, "Remove desktop state registry key");
    }
}

std::wstring wide(const std::u16string &value)
{
    return {value.begin(), value.end()};
}

bool sameRegistryName(const std::u16string &first, const std::wstring &second)
{
    return CompareStringOrdinal(wide(first).c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::vector<std::wstring> registryNames(HKEY key, bool values)
{
    std::array<wchar_t, 16384> name;
    std::vector<std::wstring> names;
    for (DWORD index = 0;; ++index) {
        DWORD size = static_cast<DWORD>(name.size());
        const auto error = values ? RegEnumValueW(key, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr)
                                  : RegEnumKeyExW(key, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (error == ERROR_NO_MORE_ITEMS) {
            break;
        }
        checkRegistry(error, "Enumerate notification registry state");
        names.emplace_back(name.data(), size);
    }
    return names;
}

ServiceState::RegistryTree captureRegistry(HKEY key, unsigned depth = 0)
{
    if (depth > 256) {
        throw std::runtime_error("Notification registry tree is too deep");
    }
    ServiceState::RegistryTree snapshot;
    for (const auto &name : registryNames(key, true)) {
        ServiceState::Value value;
        value.name = {name.begin(), name.end()};
        DWORD bytes = 0;
        DWORD type = 0;
        checkRegistry(RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes), "Measure notification registry value");
        if (bytes > ServiceState::maximumSize) {
            throw std::runtime_error("Notification registry value is too large");
        }
        value.data.resize(std::max<DWORD>(bytes, 1));
        bytes = static_cast<DWORD>(value.data.size());
        checkRegistry(RegQueryValueExW(key, name.c_str(), nullptr, &type, value.data.data(), &bytes), "Read notification registry value");
        value.data.resize(bytes);
        value.type = type;
        snapshot.values.push_back(std::move(value));
    }
    for (const auto &name : registryNames(key, false)) {
        auto child = openKey(key, name);
        if (!child) {
            throw std::runtime_error("Notification registry changed during backup; stop KDE Connect and retry");
        }
        snapshot.children.emplace(std::u16string(name.begin(), name.end()), captureRegistry(child.get(), depth + 1));
    }
    return snapshot;
}

void restoreRegistry(const ServiceState::RegistryTree &original, HKEY current)
{
    for (const auto &name : registryNames(current, true)) {
        if (std::ranges::none_of(original.values, [&](const auto &value) {
                return sameRegistryName(value.name, name);
            })) {
            checkRegistry(RegDeleteValueW(current, name.c_str()), "Remove service-created notification registry value");
        }
    }
    for (const auto &name : registryNames(current, false)) {
        if (std::ranges::none_of(original.children, [&](const auto &child) {
                return sameRegistryName(child.first, name);
            })) {
            deleteKey(current, name);
        }
    }
    for (const auto &value : original.values) {
        checkRegistry(RegSetValueExW(current, wide(value.name).c_str(), 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size())),
                      "Restore notification registry value");
    }
    for (const auto &[name, child] : original.children) {
        auto destination = openKey(current, wide(name), true);
        restoreRegistry(child, destination.get());
    }
}

std::filesystem::path resolveLocalAppData()
{
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    if (FAILED(result)) {
        CoTaskMemFree(raw);
        throw std::system_error(result, std::system_category(), "Locate user desktop state directory");
    }
    const std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

const std::filesystem::path &userConfigDirectory()
{
    static const auto config = resolveLocalAppData() / L"kdeconnect";
    return config;
}

class UserStateScope
{
public:
    UserStateScope()
    {
        HANDLE raw = nullptr;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT, &raw), "Open registration user token");
        Handle token(raw);
        const auto sid = tokenSid(token.get());
        HKEY user = nullptr;
        checkRegistry(RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &user), "Open registered user hive by SID");
        m_user.reset(user);
        userConfigDirectory();
        // A linked UAC token can be identification-only. Restrict our own primary token instead.
        HANDLE restrictedRaw = nullptr;
        check(CreateRestrictedToken(token.get(), LUA_TOKEN | DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restrictedRaw),
              "Create restricted user state token");
        Handle restricted(restrictedRaw);
        BYTE mediumSid[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(mediumSid);
        check(CreateWellKnownSid(WinMediumLabelSid, nullptr, mediumSid, &sidSize), "Create user state integrity SID");
        TOKEN_MANDATORY_LABEL label{{mediumSid, SE_GROUP_INTEGRITY}};
        check(SetTokenInformation(restricted.get(), TokenIntegrityLevel, &label, sizeof(label) + sidSize), "Set user state integrity level");
        HANDLE impersonationRaw = nullptr;
        check(DuplicateTokenEx(restricted.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &impersonationRaw),
              "Create user state impersonation token");
        Handle impersonation(impersonationRaw);
        check(ImpersonateLoggedOnUser(impersonation.get()), "Use desktop permissions for user state");
        m_impersonating = true;
    }
    ~UserStateScope()
    {
        if (m_impersonating) {
            RevertToSelf();
        }
    }

    HKEY registry() const
    {
        return m_user.get();
    }

private:
    RegistryHandle m_user;
    bool m_impersonating = false;
};

std::array<std::wstring, ServiceState::registryCount> desktopRegistryKeys()
{
    return {std::wstring(L"Software\\Classes\\AppUserModelId\\") + WindowsDesktop::appId,
            std::wstring(L"Software\\Classes\\CLSID\\") + WindowsDesktop::activatorId,
            std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\") + WindowsDesktop::appId};
}

std::array<std::filesystem::path, ServiceState::fileCount> desktopFiles()
{
    const auto &config = userConfigDirectory();
    return {config / L"background.log",
            config / L"background.log.1",
            config / L"background-status.json",
            config / L"transfer-notifications.log",
            config / L"transfer-notifications.log.1"};
}

void removeFile(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        const DWORD writable = attributes & ~FILE_ATTRIBUTE_READONLY;
        check(SetFileAttributesW(path.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL), "Clear desktop state file attributes");
    }
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        check(FALSE, "Remove desktop state file");
    }
}

void removeEmptyDirectory(const std::filesystem::path &path)
{
    if (!RemoveDirectoryW(path.c_str())) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && error != ERROR_DIR_NOT_EMPTY) {
            throw std::system_error(error, std::system_category(), "Remove empty service directory");
        }
    }
}

std::filesystem::path stateDirectory()
{
    return userConfigDirectory() / ServiceState::directoryName;
}

std::filesystem::path stateFile()
{
    return stateDirectory() / ServiceState::fileName;
}

bool regularStateFile(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        throw std::system_error(error, std::system_category(), "Inspect background service state file");
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("Background service state files must not be directories or reparse points");
    }
    return true;
}

ServiceState::Snapshot readSnapshot(const std::filesystem::path &path)
{
    Handle input(CreateFileW(path.c_str(),
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr));
    check(input.get() != INVALID_HANDLE_VALUE, "Open background service state file");
    LARGE_INTEGER size{};
    check(GetFileSizeEx(input.get(), &size), "Measure background service state file");
    if (size.QuadPart < static_cast<LONGLONG>(ServiceState::minimumSize)) {
        throw std::runtime_error("Truncated background service state file");
    }
    if (size.QuadPart > static_cast<LONGLONG>(ServiceState::maximumSize)) {
        throw std::runtime_error("Background service state file is too large");
    }
    std::vector<std::uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD bytes = 0;
    check(ReadFile(input.get(), data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr), "Read background service state file");
    if (bytes != data.size()) {
        throw std::runtime_error("Incomplete background service state read");
    }
    return ServiceState::decode(data);
}

void writeSnapshot(const ServiceState::Snapshot &snapshot)
{
    const auto data = ServiceState::encode(snapshot);
    const auto directory = stateDirectory();
    std::filesystem::create_directories(directory);
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("Background service state directory must not be a reparse point");
    }
    const auto temporary = directory / L"state.bin.new";
    if (std::filesystem::exists(temporary)) {
        regularStateFile(temporary);
    }
    try {
        Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        check(output.get() != INVALID_HANDLE_VALUE, "Create temporary background service state");
        DWORD written = 0;
        check(WriteFile(output.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr), "Write background service state");
        if (written != data.size()) {
            throw std::runtime_error("Incomplete background service state write");
        }
        check(FlushFileBuffers(output.get()), "Flush background service state");
        output.reset();
        check(MoveFileExW(temporary.c_str(), stateFile().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Publish background service state");
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

std::optional<ServiceState::Snapshot> loadDesktopState()
{
    if (regularStateFile(stateFile())) {
        return readSnapshot(stateFile());
    }
    const auto temporary = stateDirectory() / L"state.bin.new";
    if (regularStateFile(temporary)) {
        auto snapshot = readSnapshot(temporary);
        if (snapshot.flags & ServiceState::Complete) {
            throw std::runtime_error("An unpublished service snapshot needs recovery; do not overwrite it");
        }
        writeSnapshot(snapshot);
        return snapshot;
    }
    if (std::filesystem::exists(stateDirectory()) && !std::filesystem::is_empty(stateDirectory())) {
        throw std::runtime_error("Refusing to overwrite an unrelated service-state directory");
    }
    return {};
}

void saveDesktopState(bool hasService, bool registering)
{
    UserStateScope scope;
    auto previous = loadDesktopState();
    if (hasService && (!previous || !(previous->flags & ServiceState::Complete))) {
        throw std::runtime_error("Existing service requires a complete recovery snapshot");
    }
    if (previous && registering && (previous->flags & (ServiceState::Removing | ServiceState::Restored))) {
        throw std::runtime_error("Previous service removal cleanup is incomplete; run --service-unregister before registering again");
    }
    if (previous && (previous->flags & ServiceState::Complete)) {
        return;
    }
    ServiceState::Snapshot snapshot;
    const auto config = stateDirectory().parent_path();
    if (!std::filesystem::exists(config) || (previous && (previous->flags & ServiceState::ConfigCreated))) {
        snapshot.flags |= ServiceState::ConfigCreated;
    }
    writeSnapshot(snapshot);
    const auto keys = desktopRegistryKeys();
    for (size_t index = 0; index < keys.size(); ++index) {
        auto original = openKey(scope.registry(), keys[index]);
        if (original) {
            snapshot.registry[index] = captureRegistry(original.get());
        }
    }
    const auto files = desktopFiles();
    for (size_t index = 0; index < files.size(); ++index) {
        const DWORD attributes = GetFileAttributesW(files[index].c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            check(FALSE, "Read original desktop state file attributes");
        }
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                throw std::runtime_error("Desktop state files must not be directories or reparse points");
            }
            const auto backup = stateDirectory() / std::to_wstring(index);
            removeFile(backup);
            std::filesystem::copy_file(files[index], backup);
        }
        snapshot.attributes[index] = attributes;
    }
    snapshot.flags |= ServiceState::Complete;
    writeSnapshot(snapshot);
}

void clearServiceNotifications(HKEY user)
{
    auto identity = openKey(user, desktopRegistryKeys()[0]);
    DWORD valueSize = 0;
    if (!identity || RegGetValueW(identity.get(), nullptr, L"CustomActivator", RRF_RT_REG_SZ, nullptr, nullptr, &valueSize) != ERROR_SUCCESS) {
        return;
    }
#ifdef _MSC_VER
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        throw std::system_error(initialized, std::system_category(), "Initialize notification history cleanup");
    }
    struct Apartment {
        bool owned;
        ~Apartment()
        {
            if (owned) {
                RoUninitialize();
            }
        }
    } apartment{SUCCEEDED(initialized)};
    try {
        const auto history = winrt::Windows::UI::Notifications::ToastNotificationManager::History();
        history.RemoveGroup(WindowsDesktop::serviceGroup, WindowsDesktop::appId);
    } catch (const winrt::hresult_error &error) {
        if (error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            throw std::system_error(error.code().value, std::system_category(), "Clear service notification history");
        }
    }
#endif
}

void restoreDesktopState()
{
    UserStateScope scope;
    auto state = loadDesktopState();
    const auto files = desktopFiles();
    if (!state) {
        removeEmptyDirectory(stateDirectory());
        return;
    }
    const auto backup = stateDirectory();
    const auto config = backup.parent_path();
    if ((state->flags & ServiceState::Complete) && !(state->flags & ServiceState::Restored)) {
        const auto keys = desktopRegistryKeys();
        for (size_t index = 0; index < files.size(); ++index) {
            if (state->attributes[index] != INVALID_FILE_ATTRIBUTES) {
                if (!regularStateFile(backup / std::to_wstring(index))) {
                    throw std::runtime_error("A desktop state file backup is missing; desktop state was not restored");
                }
            }
        }
        clearServiceNotifications(scope.registry());
        for (size_t index = 0; index < keys.size(); ++index) {
            const auto &original = state->registry[index];
            if (original) {
                auto destination = openKey(scope.registry(), keys[index], true);
                restoreRegistry(*original, destination.get());
            } else {
                deleteKey(scope.registry(), keys[index]);
            }
        }
        for (size_t index = 0; index < files.size(); ++index) {
            const auto original = backup / std::to_wstring(index);
            const DWORD attributes = state->attributes[index];
            removeFile(files[index]);
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                std::filesystem::create_directories(files[index].parent_path());
                std::filesystem::copy_file(original, files[index]);
                check(SetFileAttributesW(files[index].c_str(), attributes), "Restore desktop state file attributes");
            }
        }
        state->flags |= ServiceState::Restored;
        writeSnapshot(*state);
    }
    for (size_t index = 0; index < files.size(); ++index) {
        removeFile(backup / std::to_wstring(index));
    }
    removeFile(backup / L"state.bin.new");
    removeFile(stateFile());
    try {
        removeEmptyDirectory(backup);
        if (state->flags & ServiceState::ConfigCreated) {
            removeEmptyDirectory(config);
        }
    } catch (...) {
        // Keep the startup gate and retry state if final directory cleanup fails.
        writeSnapshot(*state);
        throw;
    }
}

void removeHostFiles(const std::wstring &sid)
{
    const auto directory = hostDirectory(sid, false);
    for (const auto *name : {L"kdeconnect-service.exe", L"kdeconnect-service.exe.new", L"service.log", L"service.log.1"}) {
        removeFile(directory / name);
    }
    removeEmptyDirectory(directory);
    removeEmptyDirectory(directory.parent_path());
}

void configureService(bool install)
{
    if (!IsUserAnAdmin()) {
        throw std::runtime_error("Run kdeconnect-cli from an elevated terminal as the user whose background service you want to configure");
    }
    const auto sid = currentSid();
    const auto source = executablePath();
    const auto daemon = source.parent_path() / L"kdeconnectd.exe";
    if (install && !std::filesystem::is_regular_file(daemon)) {
        throw std::runtime_error("kdeconnectd.exe must be next to the bundled service helper");
    }
    const auto name = serviceName(sid);
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | (install ? SC_MANAGER_CREATE_SERVICE : 0)));
    if (!manager) {
        throw std::runtime_error("Run kdeconnect-cli from an elevated terminal as the user whose background service you want to configure");
    }
    auto descriptor = securityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    Handle mutex(CreateMutexW(&attributes, FALSE, (L"Global\\" + name + L"-install").c_str()));
    check(mutex.get() != nullptr, "Create registration lock");
    const DWORD lock = WaitForSingleObject(mutex.get(), 45000);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) {
        throw std::runtime_error("Another service registration operation is in progress");
    }
    struct Unlock {
        HANDLE handle;
        ~Unlock()
        {
            ReleaseMutex(handle);
        }
    } unlock{mutex.get()};

    ServiceHandle service(OpenServiceW(manager.get(), name.c_str(), SERVICE_ALL_ACCESS));
    if (!service && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        check(FALSE, "Open background service");
    }
    const bool hadService = bool(service);
    if (install || hadService) {
        saveDesktopState(hadService, install);
    }
    if (!install) {
        UserStateScope scope;
        if (auto state = loadDesktopState()) {
            state->flags |= ServiceState::Removing;
            writeSnapshot(*state);
        }
    }
    if (service) {
        stopService(service.get());
    }
    if (!install) {
        if (service) {
            check(DeleteService(service.get()), "Delete service");
            service.reset();
        }
        removeHostFiles(sid);
        restoreDesktopState();
        std::wcout << L"Background service and its desktop integration removed. User configuration and pairing data were retained.\n";
        return;
    }

    const auto directory = hostDirectory(sid, true);
    const auto destination = directory / L"kdeconnect-service.exe";
    if (source != destination) {
        const auto temporary = directory / L"kdeconnect-service.exe.new";
        copyHost(source, temporary);
        bool moved = false;
        for (int attempt = 0; attempt < 40 && !moved; ++attempt) {
            moved = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!moved) {
                Sleep(100);
            }
        }
        check(moved, "Install service host");
    }
    const std::wstring command = quote(destination.wstring()) + L" --run " + quote(sid);
    const std::wstring display = L"KDE Connect Background (" + sid + L")";
    bool created = false;
    if (!service) {
        service.reset(CreateServiceW(manager.get(),
                                     name.c_str(),
                                     display.c_str(),
                                     SERVICE_ALL_ACCESS,
                                     SERVICE_WIN32_OWN_PROCESS,
                                     SERVICE_AUTO_START,
                                     SERVICE_ERROR_NORMAL,
                                     command.c_str(),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr));
        check(service.get() != nullptr, "Create service");
        created = true;
    }
    try {
        check(ChangeServiceConfigW(service.get(),
                                   SERVICE_NO_CHANGE,
                                   SERVICE_AUTO_START,
                                   SERVICE_NO_CHANGE,
                                   command.c_str(),
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   L"LocalSystem",
                                   nullptr,
                                   display.c_str()),
              "Configure service");
        std::wstring description = L"Starts KDE Connect without an app or tray after this user logs in, and recovers interrupted background processes.";
        SERVICE_DESCRIPTIONW serviceDescription{description.data()};
        check(ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &serviceDescription), "Set service description");
        SC_ACTION actions[]{{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_RESTART, 60000}};
        SERVICE_FAILURE_ACTIONSW recovery{3600, nullptr, nullptr, 3, actions};
        check(ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &recovery), "Configure service recovery");
        SERVICE_FAILURE_ACTIONS_FLAG recoveryFlag{TRUE};
        check(ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &recoveryFlag), "Enable service recovery");
        saveDaemonPath(sid, daemon.wstring());
        check(StartServiceW(service.get(), 0, nullptr), "Start service");
        waitForState(service.get(), SERVICE_RUNNING);
    } catch (...) {
        if (created) {
            SERVICE_STATUS status{};
            ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
            DeleteService(service.get());
        }
        throw;
    }
    std::wcout << L"Registered " << name << L". The service starts at boot; KDE Connect runs when this user is logged in.\n";
}

std::wstring targetSid;
SERVICE_STATUS_HANDLE statusHandle = nullptr;
Handle serviceStop;
Handle sessionChanged;
std::filesystem::path logPath;

void log(const std::string &message)
{
    std::error_code error;
    if (std::filesystem::file_size(logPath, error) > 1024 * 1024 && !error) {
        auto previous = logPath;
        previous += L".1";
        DeleteFileW(previous.c_str());
        MoveFileW(logPath.c_str(), previous.c_str());
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char timestamp[40];
    snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02uZ ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::ofstream output(logPath, std::ios::app);
    output << timestamp << message << '\n';
}

void reportStatus(DWORD state, DWORD error = NO_ERROR)
{
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    if (state == SERVICE_RUNNING) {
        status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    } else if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        status.dwWaitHint = 15000;
        status.dwCheckPoint = 1;
    }
    SetServiceStatus(statusHandle, &status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, void *, void *)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetEvent(serviceStop.get());
    } else if (control == SERVICE_CONTROL_SESSIONCHANGE) {
        SetEvent(sessionChanged.get());
    } else if (control != SERVICE_CONTROL_INTERROGATE) {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    return NO_ERROR;
}

Handle sessionToken(DWORD preferredSession, DWORD &selectedSession)
{
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    check(WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count), "Enumerate user sessions");
    std::vector<WTS_SESSION_INFOW> candidates(sessions, sessions + count);
    WTSFreeMemory(sessions);
    std::stable_sort(candidates.begin(), candidates.end(), [preferredSession](const auto &a, const auto &b) {
        const int first = a.SessionId == preferredSession ? 0 : (a.State == WTSActive ? 1 : 2);
        const int second = b.SessionId == preferredSession ? 0 : (b.State == WTSActive ? 1 : 2);
        return first < second;
    });
    for (const auto &session : candidates) {
        if (!session.SessionId || (session.State != WTSActive && session.State != WTSConnected && session.State != WTSDisconnected)) {
            continue;
        }
        HANDLE raw = nullptr;
        if (!WTSQueryUserToken(session.SessionId, &raw)) {
            continue;
        }
        Handle token(raw);
        if (tokenSid(token.get()) == targetSid) {
            // An administrator's worker still runs with the filtered desktop token.
            TOKEN_ELEVATION_TYPE type{};
            DWORD size = 0;
            check(GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size), "Query user token elevation");
            if (type == TokenElevationTypeFull) {
                TOKEN_LINKED_TOKEN linked{};
                check(GetTokenInformation(token.get(), TokenLinkedToken, &linked, sizeof(linked), &size), "Get desktop user token");
                token.reset(linked.LinkedToken);
            }
            selectedSession = session.SessionId;
            return token;
        }
    }
    selectedSession = MAXDWORD;
    return {};
}

struct Worker {
    Handle process;
    Handle job;
    Handle heartbeat;
    Handle stop;
    DWORD session = MAXDWORD;
    ULONGLONG started = 0;
    ULONGLONG lastHeartbeat = 0;

    void finish()
    {
        if (process) {
            SetEvent(stop.get());
            if (WaitForSingleObject(process.get(), 10000) != WAIT_OBJECT_0) {
                TerminateJobObject(job.get(), ERROR_TIMEOUT);
                WaitForSingleObject(process.get(), 5000);
            }
            process.reset();
        }
        job.reset();
        heartbeat.reset();
        stop.reset();
        session = MAXDWORD;
    }

    ~Worker()
    {
        finish();
    }

    void start(HANDLE token, DWORD userSession, const std::wstring &daemon)
    {
        auto descriptor = securityDescriptor(L"D:P(A;;GA;;;SY)(A;;0x00100002;;;" + targetSid + L")");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            throw std::runtime_error("Cannot create supervision identifier");
        }
        wchar_t identifier[40];
        StringFromGUID2(guid, identifier, 40);
        const std::wstring prefix = L"Global\\KDEConnectBackground-" + std::wstring(identifier);
        const std::wstring heartbeatName = prefix + L"-heartbeat";
        const std::wstring stopName = prefix + L"-stop";
        heartbeat.reset(CreateEventW(&attributes, FALSE, FALSE, heartbeatName.c_str()));
        stop.reset(CreateEventW(&attributes, TRUE, FALSE, stopName.c_str()));
        check(heartbeat && stop, "Create supervision events");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        check(job.get() != nullptr, "Create worker job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Set worker job limits");
        void *environment = nullptr;
        check(CreateEnvironmentBlock(&environment, token, FALSE), "Create user environment");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        wchar_t desktop[] = L"winsta0\\default";
        startup.lpDesktop = desktop;
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        std::wstring command = quote(daemon) + L" --background --replace --service-heartbeat " + quote(heartbeatName) + L" --service-stop " + quote(stopName);
        const auto workingDirectory = std::filesystem::path(daemon).parent_path();
        PROCESS_INFORMATION information{};
        const BOOL created = CreateProcessAsUserW(token,
                                                  daemon.c_str(),
                                                  command.data(),
                                                  nullptr,
                                                  nullptr,
                                                  FALSE,
                                                  CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                                  environment,
                                                  workingDirectory.c_str(),
                                                  &startup,
                                                  &information);
        const DWORD error = GetLastError();
        DestroyEnvironmentBlock(environment);
        if (!created) {
            throw std::system_error(error, std::system_category(), "Start user background process");
        }
        process.reset(information.hProcess);
        Handle thread(information.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == MAXDWORD) {
            const DWORD assignmentError = GetLastError();
            TerminateProcess(process.get(), assignmentError);
            throw std::system_error(assignmentError, std::system_category(), "Assign background process to job");
        }
        session = userSession;
        started = lastHeartbeat = awakeMilliseconds();
        log("Started user background process PID=" + std::to_string(information.dwProcessId) + " session=" + std::to_string(session));
    }
};

void supervise()
{
    const auto daemon = loadDaemonPath(targetSid);
    Worker worker;
    ULONGLONG retryAt = 0;
    ULONGLONG delay = 5000;
    while (WaitForSingleObject(serviceStop.get(), 0) != WAIT_OBJECT_0) {
        try {
            DWORD session = MAXDWORD;
            auto token = sessionToken(worker.session, session);
            if (worker.process && (!token || worker.session != session)) {
                log("User session ended; stopping owned background process");
                worker.finish();
                retryAt = 0;
                delay = 5000;
            }
            const ULONGLONG now = awakeMilliseconds();
            if (worker.process) {
                if (WaitForSingleObject(worker.heartbeat.get(), 0) == WAIT_OBJECT_0) {
                    worker.lastHeartbeat = now;
                }
                if (WaitForSingleObject(worker.process.get(), 0) == WAIT_OBJECT_0 || now - worker.lastHeartbeat > 60000) {
                    if (now - worker.started > 300000) {
                        delay = 5000;
                    }
                    log("Background process exited or stopped responding; scheduling recovery");
                    worker.finish();
                    retryAt = awakeMilliseconds() + delay;
                    delay = std::min<ULONGLONG>(delay * 2, 120000);
                }
            }
            if (token && !worker.process && awakeMilliseconds() >= retryAt) {
                worker.start(token.get(), session, daemon);
            }
        } catch (const std::exception &error) {
            log(error.what());
            worker.finish();
            retryAt = awakeMilliseconds() + delay;
            delay = std::min<ULONGLONG>(delay * 2, 120000);
        }
        HANDLE events[]{serviceStop.get(), sessionChanged.get()};
        WaitForMultipleObjects(2, events, FALSE, 2000);
    }
    reportStatus(SERVICE_STOP_PENDING);
    worker.finish();
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    const auto name = serviceName(targetSid);
    serviceStop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    sessionChanged.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    statusHandle = RegisterServiceCtrlHandlerExW(name.c_str(), controlHandler, nullptr);
    if (!statusHandle) {
        return;
    }
    reportStatus(SERVICE_START_PENDING);
    try {
        check(serviceStop && sessionChanged, "Create service control events");
        logPath = hostDirectory(targetSid, false) / L"service.log";
        loadDaemonPath(targetSid);
        reportStatus(SERVICE_RUNNING);
        log("Service running; waiting for registered user's session");
        supervise();
        log("Service stopped");
        reportStatus(SERVICE_STOPPED);
    } catch (const std::exception &error) {
        log(error.what());
        reportStatus(SERVICE_STOPPED, ERROR_EXCEPTION_IN_SERVICE);
    }
}
}

int wmain(int argc, wchar_t **argv)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        if (argc == 2 && (std::wstring(argv[1]) == L"--register" || std::wstring(argv[1]) == L"--unregister")) {
            configureService(std::wstring(argv[1]) == L"--register");
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--run") {
            targetSid = argv[2];
            PSID raw = nullptr;
            check(ConvertStringSidToSidW(targetSid.c_str(), &raw), "Validate service user SID");
            LocalMemory sid(raw);
            std::wstring name = serviceName(targetSid);
            SERVICE_TABLE_ENTRYW table[]{{name.data(), serviceMain}, {nullptr, nullptr}};
            check(StartServiceCtrlDispatcherW(table), "Connect to Service Control Manager");
            return 0;
        }
        std::wcerr << L"Usage: kdeconnect-service --register | --unregister (use kdeconnect-cli for service management)\n";
        return 1;
    } catch (const std::system_error &error) {
        std::cerr << error.what() << " (" << error.code() << ")\n";
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
