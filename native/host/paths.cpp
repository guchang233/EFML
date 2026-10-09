#include "paths.h"

#include <Windows.h>

#include <vector>

namespace EFML::Host {
namespace {

std::wstring Environment(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size == 0 || size >= buffer.size() ? std::wstring() : std::wstring(buffer.data(), size);
}

} // namespace

std::filesystem::path DefaultDataRoot() {
    const auto home = Environment(L"EFML_HOME");
    if (!home.empty()) return home;
    const auto local = Environment(L"LOCALAPPDATA");
    return local.empty() ? std::filesystem::path() : std::filesystem::path(local) / L"EFML";
}

Paths DiscoverPaths(const wchar_t* bootstrap_data_root) {
    Paths paths;
    paths.data_root = bootstrap_data_root && *bootstrap_data_root
        ? std::filesystem::path(bootstrap_data_root) : DefaultDataRoot();
    paths.index_file = paths.data_root / L"modules" / L"index.json";
    paths.log_root = paths.data_root / L"logs";
    std::error_code ignored;
    std::filesystem::create_directories(paths.log_root, ignored);
    std::filesystem::create_directories(paths.index_file.parent_path(), ignored);
    return paths;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(ToWide(value));
}

} // namespace EFML::Host
