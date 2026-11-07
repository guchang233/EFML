#pragma once
// Official-launcher launch bridge, derived from Better-Endfield's
// native/loaders/shared/official_launcher.h (Better Endfield Next 4.1.4, AGPL-3.0).
//
// Endfield 1.6.3 runs a launch check in OneChannelLC.dll: it reads the *direct* parent process
// image path, compares the file name with the configured allow list (Games.exe), validates that
// file's Authenticode signature and matches the signer certificate fingerprint. A real, signed
// official Games.exe started as a suspended process and passed as
// PROC_THREAD_ATTRIBUTE_PARENT_PROCESS satisfies all three without running the launcher UI.
//
// This header is shared by EFML.Launcher.exe and EFML.Injector.exe; it only creates processes.
#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace EFML::Loader::Launch {

struct Options {
    std::filesystem::path game;
    std::filesystem::path parent;
    std::filesystem::path home;
    std::vector<std::wstring> arguments;
    bool no_parent = false;
    bool hide_console = false;
    bool check = false;
};

// Parses "--game <Endfield.exe> [--parent <Games.exe>] [--home <dir>] [--hide-console]
// [--no-parent] [--check] [-- <game arguments>]".
bool ParseOptions(int argc, wchar_t* argv[], Options& options, std::wstring& error);

std::wstring QuoteArgument(const std::wstring& argument);
std::wstring WindowsError(const wchar_t* operation, DWORD code);
void ShowError(const std::wstring& message);

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr);
private:
    HANDLE value_ = nullptr;
};

struct VerifiedLauncher {
    std::filesystem::path executable;
    // Organization from the signer certificate, empty when it could not be read.
    std::wstring publisher;
    // Deny replacement from signature verification through process creation.
    Handle file;
};

// Finds the installed official launcher: registry protocol handlers, uninstall entries and the
// usual install folders, newest version first. Each candidate must be a valid PE and carry a
// trusted Authenticode signature from Hypergryph/Gryphline; the first match wins.
bool FindOfficialLauncher(const std::filesystem::path& requested, VerifiedLauncher& result,
    std::wstring& error);

// Owns the suspended official launcher process used as the game's parent. The primary thread is
// never resumed, so the launcher UI and its single-instance mutex never run.
class OfficialParent {
public:
    OfficialParent() = default;
    ~OfficialParent();
    OfficialParent(const OfficialParent&) = delete;
    OfficialParent& operator=(const OfficialParent&) = delete;

    bool Start(const VerifiedLauncher& launcher, std::wstring& error);
    bool CreateGame(const std::filesystem::path& game,
        const std::vector<std::wstring>& arguments, DWORD creation_flags,
        PROCESS_INFORMATION& process, std::wstring& error) const;
    DWORD pid() const { return pid_; }
private:
    Handle process_;
    DWORD pid_ = 0;
};

// Starts the game without borrowing a parent (-no-parent). The client's launcher check will
// reject this on the CN client; it is kept for clients without the check and for diagnostics.
bool CreateGameProcess(const std::filesystem::path& game,
    const std::vector<std::wstring>& arguments, DWORD creation_flags,
    PROCESS_INFORMATION& process, std::wstring& error);

bool WaitForGame(HANDLE process, DWORD& exit_code, std::wstring& error);

} // namespace EFML::Loader::Launch
