// EFML.Launcher.exe — starts Endfield.exe with the installed official Hypergryph launcher as its
// direct parent, so the client's launcher check (OneChannelLC.dll: parent name + Authenticode
// signature + signer fingerprint) passes without showing the launcher UI. The game still loads
// EFML through the xinput1_4.dll proxy; this helper only fixes the parent identity.
//
// Derived from Better-Endfield's native/loaders/launcher/main.cpp (Better Endfield Next 4.1.4,
// AGPL-3.0). EFML adds --home (per-instance EFML_HOME for dual clients) and --check.
#include "loader_config.h"
#include "official_launcher.h"

#include <Windows.h>

#include <iostream>
#include <locale>
#include <string>

namespace {

using namespace EFML::Loader::Launch;

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        output.data(), size, nullptr, nullptr);
    return output;
}

void Log(const std::wstring& text) {
    EFML::Loader::LoaderLog(EFML::Loader::DataRoot(), "launcher: " + Narrow(text));
}

// Ends the process this invocation created if startup never completed; once the game runs it only
// closes the handles.
class OwnedGame {
public:
    explicit OwnedGame(const PROCESS_INFORMATION& process) noexcept
        : process_(process.hProcess), thread_(process.hThread), pid_(process.dwProcessId) {}
    OwnedGame(const OwnedGame&) = delete;
    OwnedGame& operator=(const OwnedGame&) = delete;
    ~OwnedGame() {
        if (!started_ && process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
        if (thread_) CloseHandle(thread_);
        if (process_) CloseHandle(process_);
    }
    void Started() noexcept {
        started_ = true;
        if (thread_) { CloseHandle(thread_); thread_ = nullptr; }
    }
    HANDLE process() const noexcept { return process_; }
    DWORD pid() const noexcept { return pid_; }
private:
    HANDLE process_;
    HANDLE thread_;
    DWORD pid_;
    bool started_ = false;
};

int Check(const Options& options, const std::wstring& home, const VerifiedLauncher& launcher) {
    std::wcout << L"game       " << options.game.wstring() << L"\n";
    std::wcout << L"home       " << home << L"\n";
    if (options.no_parent) {
        std::wcout << L"parent     (none: --no-parent)\n";
    } else {
        std::wcout << L"parent     " << launcher.executable.wstring() << L"\n";
        std::wcout << L"publisher  " << (launcher.publisher.empty() ? L"(unknown)" : launcher.publisher) << L"\n";
    }
    return 0;
}

DWORD Run(int argc, wchar_t* argv[]) {
    Options options;
    std::wstring error;
    if (!ParseOptions(argc, argv, options, error)) {
        ShowError(error);
        return 2;
    }
    // Console callers keep their diagnostics; the manager passes --hide-console.
    if (options.hide_console) FreeConsole();
    if (!options.home.empty()) SetEnvironmentVariableW(L"EFML_HOME", options.home.c_str());
    const auto home = EFML::Loader::DataRoot();
    // --check stays read-only: no log file, no game process.
    if (!options.check) Log(L"game=" + options.game.wstring() + L" home=" + home.wstring());

    VerifiedLauncher launcher;
    if (!options.no_parent) {
        if (!FindOfficialLauncher(options.parent, launcher, error)) {
            if (options.check) {
                std::wcerr << error << L"\n";
                return 7;
            }
            ShowError(error);
            return 7;
        }
    }

    if (options.check) return Check(options, home, launcher);

    PROCESS_INFORMATION process{};
    OfficialParent parent;
    if (!options.no_parent) {
        if (!parent.Start(launcher, error)) {
            ShowError(error);
            return 4;
        }
        Log(L"suspended official parent pid " + std::to_wstring(parent.pid()));
        // The parent image and its signature are established; let the official updater touch it.
        launcher.file.reset();
        if (!parent.CreateGame(options.game, options.arguments, CREATE_SUSPENDED, process, error)) {
            ShowError(error);
            return 4;
        }
    } else if (!CreateGameProcess(options.game, options.arguments, CREATE_SUSPENDED, process, error)) {
        ShowError(error);
        return 4;
    }

    OwnedGame game(process);
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        ShowError(WindowsError(L"恢复游戏线程 / Resume game", GetLastError()));
        return 4;
    }
    game.Started();
    Log(L"game pid " + std::to_wstring(game.pid()) + L" started; waiting for exit");

    DWORD exit_code = 0;
    if (!WaitForGame(game.process(), exit_code, error)) {
        ShowError(error);
        return 8;
    }
    Log(L"game pid " + std::to_wstring(game.pid()) + L" exited with " + std::to_wstring(exit_code));
    return exit_code;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Wide diagnostics are printed as UTF-8 bytes; the CLI sets its console to UTF-8 as well.
    try { std::locale::global(std::locale(".UTF-8")); } catch (const std::exception&) {}
    DWORD code = 4;
    try {
        code = Run(argc, argv);
    } catch (const std::exception&) {
        ShowError(L"启动时发生异常，请检查游戏和官方启动器安装。 / "
                  L"Launch failed; check the game and the official launcher installation.");
    }
    // ExitProcess does not flush the C++ streams, and --check is read by callers through a pipe.
    std::wcout.flush();
    std::wcerr.flush();
    ExitProcess(code);
}
