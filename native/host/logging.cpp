#include "logging.h"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iomanip>

namespace EFML::Host {

void Logger::Initialize(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    file_path_ = root / "EFML.log";
    // One log per game session; a second Host in the same session would append.
    if (std::filesystem::exists(file_path_, error)) {
        std::filesystem::rename(file_path_, root / "EFML.previous.log", error);
    }
}

void Logger::Write(const std::string& source, const std::string& message) {
    std::lock_guard lock(mutex_);
    std::ofstream stream(file_path_, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream) return;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm time_info{};
    localtime_s(&time_info, &now);
    stream << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S") << ' ' << GetCurrentProcessId()
           << " [" << source << "] " << message << "\r\n";
}

} // namespace EFML::Host
