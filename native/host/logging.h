#pragma once
#include <filesystem>
#include <mutex>
#include <string>

namespace EFML::Host {

// Appends "YYYY-mm-dd HH:MM:SS pid [source] message" lines to logs\EFML.log. The previous
// session's log is kept as EFML.previous.log.
class Logger {
public:
    void Initialize(const std::filesystem::path& root);
    void Write(const std::string& source, const std::string& message);
    const std::filesystem::path& File() const { return file_path_; }

private:
    std::mutex mutex_;
    std::filesystem::path file_path_;
};

} // namespace EFML::Host
