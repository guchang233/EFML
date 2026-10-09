#pragma once
// Where the loaders find the Host. %LOCALAPPDATA%\EFML\loader.ini (or %EFML_HOME%\loader.ini):
//   [Loader]
//   host=C:\...\EFML\runtime\1.0.0\EFML.Host.dll
//   load_host=true
// The manager writes this file when it installs a runtime.
#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace EFML::Loader {

inline std::wstring Environment(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size == 0 || size >= buffer.size() ? std::wstring() : std::wstring(buffer.data(), size);
}

inline std::filesystem::path DataRoot() {
    const auto home = Environment(L"EFML_HOME");
    if (!home.empty()) return home;
    const auto local = Environment(L"LOCALAPPDATA");
    return local.empty() ? std::filesystem::path() : std::filesystem::path(local) / L"EFML";
}

inline std::wstring ReadSetting(const std::filesystem::path& root, const wchar_t* key, const wchar_t* fallback) {
    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(L"Loader", key, fallback, value.data(), static_cast<DWORD>(value.size()),
        (root / L"loader.ini").c_str());
    return value.data();
}

inline bool ShouldLoadHost(const std::filesystem::path& root) {
    const auto value = ReadSetting(root, L"load_host", L"true");
    return _wcsicmp(value.c_str(), L"true") == 0 || _wcsicmp(value.c_str(), L"1") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0 || _wcsicmp(value.c_str(), L"on") == 0;
}

inline std::filesystem::path HostPath(const std::filesystem::path& root) {
    return ReadSetting(root, L"host", L"");
}

// Appends one line to logs\loader.log.
inline void LoaderLog(const std::filesystem::path& root, const std::string& text) {
    if (root.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(root / L"logs", ignored);
    const HANDLE file = CreateFileW((root / L"logs" / L"loader.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char prefix[64]{};
    const int length = _snprintf_s(prefix, _countof(prefix), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u %lu ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    const std::string line = std::string(prefix, length > 0 ? length : 0) + text + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

} // namespace EFML::Loader
