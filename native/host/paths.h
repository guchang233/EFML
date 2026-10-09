#pragma once
#include <filesystem>
#include <string>

namespace EFML::Host {

// Data root, in order: the injector's bootstrap block, the EFML_HOME environment variable,
// %LOCALAPPDATA%\EFML.
//   modules\index.json          installed modules (Better-Endfield third-party index schema 1)
//   modules\packages\<generation>\
//   logs\EFML.log
struct Paths {
    std::filesystem::path data_root;
    std::filesystem::path index_file;
    std::filesystem::path log_root;
};

Paths DiscoverPaths(const wchar_t* bootstrap_data_root);
std::filesystem::path DefaultDataRoot();
std::string ToUtf8(const std::wstring& value);
std::wstring ToWide(const std::string& value);
std::filesystem::path Utf8Path(const std::string& value);

} // namespace EFML::Host
