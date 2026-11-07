// Derived from Better-Endfield's native/loaders/shared/official_launcher.cpp
// (Better Endfield Next 4.1.4, AGPL-3.0). EFML keeps the discovery/lifetime design and adds a real
// Authenticode check: Better-Endfield's header promises signature verification while its 4.1.4
// implementation only validated the PE headers, and the client's own check rejects everything else.
#include "official_launcher.h"

#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <iostream>
#include <utility>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

namespace EFML::Loader::Launch {
namespace {

constexpr wchar_t kPublisher[] = L"Shanghai Hypergryph Network Technology Co., Ltd.";
// Accept the CN (Hypergryph) and global (Gryphline) launchers. The client's own check remains the
// authority; this only keeps an obviously foreign file from being used as the parent.
constexpr const wchar_t* const kPublisherMarkers[] = {L"hypergryph", L"gryphline"};

bool Equal(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    return _wcsnicmp(text.c_str(), prefix, wcslen(prefix)) == 0;
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return text;
}

struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};

std::wstring ReadRegistry(HKEY root, const wchar_t* key, const wchar_t* name,
    REGSAM view = 0) {
    RegistryKey opened;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &opened.value) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(opened.value, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536) return {};
    std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(opened.value, name, nullptr, nullptr,
            reinterpret_cast<BYTE*>(text.data()), &bytes) != ERROR_SUCCESS) return {};
    if (type != REG_EXPAND_SZ) return text.data();
    const DWORD required = ExpandEnvironmentStringsW(text.data(), nullptr, 0);
    if (!required || required > 32768) return {};
    std::vector<wchar_t> expanded(required);
    if (!ExpandEnvironmentStringsW(text.data(), expanded.data(), required)) return {};
    return expanded.data();
}

std::vector<std::wstring> Subkeys(HKEY root, const wchar_t* path, REGSAM view = 0) {
    RegistryKey opened;
    std::vector<std::wstring> names;
    if (RegOpenKeyExW(root, path, 0, KEY_ENUMERATE_SUB_KEYS | view, &opened.value) != ERROR_SUCCESS) return names;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD size = static_cast<DWORD>(std::size(name));
        const LSTATUS status = RegEnumKeyExW(opened.value, index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) names.emplace_back(name, size);
    }
    return names;
}

void AddRoot(std::vector<std::filesystem::path>& roots, const std::filesystem::path& path) {
    if (path.empty()) return;
    auto root = path;
    if (root.has_extension() && Equal(root.extension().wstring(), L".exe")) root = root.parent_path();
    if (std::none_of(roots.begin(), roots.end(), [&](const auto& existing) { return Equal(existing.wstring(), root.wstring()); }))
        roots.push_back(root);
}

std::vector<std::filesystem::path> InstalledRoots() {
    std::vector<std::filesystem::path> roots;
    for (const auto& protocol : Subkeys(HKEY_CLASSES_ROOT, L"")) {
        if (!StartsWith(protocol, L"hypergryphlauncher") && !StartsWith(protocol, L"gryphlinklauncher")) continue;
        const auto command = ReadRegistry(HKEY_CLASSES_ROOT, (protocol + L"\\shell\\open\\command").c_str(), nullptr);
        if (command.empty()) continue;
        int count = 0;
        wchar_t** arguments = CommandLineToArgvW(command.c_str(), &count);
        if (arguments) {
            if (count > 0) AddRoot(roots, arguments[0]);
            LocalFree(arguments);
        }
    }
    constexpr wchar_t uninstall[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            for (const auto& child : Subkeys(hive, uninstall, view)) {
                const auto key = std::wstring(uninstall) + L"\\" + child;
                const auto name = ReadRegistry(hive, key.c_str(), L"DisplayName", view);
                if (!Equal(name, L"鹰角启动器") && !Equal(name, L"Hypergryph Launcher") &&
                    !Equal(name, L"GRYPHLINK") && !Equal(name, L"Gryphlink") &&
                    !Equal(name, L"Gryphline Launcher")) continue;
                AddRoot(roots, ReadRegistry(hive, key.c_str(), L"InstallLocation", view));
            }
        }
    }
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        wchar_t value[32768]{};
        const DWORD length = GetEnvironmentVariableW(variable, value, static_cast<DWORD>(std::size(value)));
        if (length && length < std::size(value)) {
            AddRoot(roots, std::filesystem::path(value) / L"Hypergryph Launcher");
            AddRoot(roots, std::filesystem::path(value) / L"GRYPHLINK");
        }
    }
    return roots;
}

std::vector<unsigned long> Version(const std::filesystem::path& path) {
    const auto name = path.parent_path().filename().wstring();
    if (name.empty() || name.find_first_not_of(L"0123456789.") != std::wstring::npos) return {};
    std::vector<unsigned long> result;
    size_t offset = 0;
    try {
        while (offset < name.size()) {
            size_t consumed = 0;
            result.push_back(std::stoul(name.substr(offset), &consumed));
            offset += consumed;
            if (offset < name.size() && name[offset++] != L'.') return {};
        }
    } catch (...) { return {}; }
    return result;
}

bool IsPublisherOrganization(const std::wstring& organization) {
    if (organization.empty()) return false;
    const auto lowered = Lower(organization);
    return std::any_of(std::begin(kPublisherMarkers), std::end(kPublisherMarkers),
        [&](const wchar_t* marker) { return lowered.find(marker) != std::wstring::npos; });
}

// Reads the organization (O=) of the certificate that produced the first signature.
bool ReadSignerOrganization(const std::wstring& path, std::wstring& organization,
    std::wstring& error) {
    DWORD encoding = 0, content = 0, format = 0;
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
            &encoding, &content, &format, &store, &message, nullptr)) {
        error = WindowsError(L"读取签名信息 / Read signature", GetLastError());
        return false;
    }
    struct Cleanup {
        HCERTSTORE store;
        HCRYPTMSG message;
        ~Cleanup() {
            if (message) CryptMsgClose(message);
            if (store) CertCloseStore(store, 0);
        }
    } cleanup{store, message};

    DWORD size = 0;
    if (!message || !CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) ||
        size < sizeof(CMSG_SIGNER_INFO)) {
        error = L"签名中没有签名者信息 / No signer information";
        return false;
    }
    std::vector<BYTE> buffer(size);
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
        error = WindowsError(L"读取签名者 / Read signer", GetLastError());
        return false;
    }
    auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(buffer.data());
    CERT_INFO info{};
    info.Issuer = signer->Issuer;
    info.SerialNumber = signer->SerialNumber;
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &info, nullptr);
    if (!certificate) {
        error = L"找不到签名者证书 / Signer certificate not found";
        return false;
    }
    const DWORD length = CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0,
        const_cast<char*>(szOID_ORGANIZATION_NAME), nullptr, 0);
    if (length > 1) {
        std::wstring value(length, L'\0');
        if (CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0,
                const_cast<char*>(szOID_ORGANIZATION_NAME), value.data(), length) == length) {
            value.resize(wcslen(value.c_str()));
            organization = value;
        }
    }
    CertFreeCertificateContext(certificate);
    return true;
}

// Authenticode check: the file must be signed by a certificate chain Windows trusts. Revocation is
// not fetched (no network dependency); the client's own check remains authoritative.
bool VerifyAuthenticode(const std::filesystem::path& path, std::wstring& publisher,
    std::wstring& error) {
    const std::wstring name = path.wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = name.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);

    if (status != ERROR_SUCCESS) {
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(status), 0,
            reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(status));
        error = L"官方启动器签名校验失败 / Launcher signature check failed (" +
            std::wstring(code) + L")";
        if (text) {
            error += L"\n" + std::wstring(text);
            LocalFree(text);
        }
        error += L"\n" + path.wstring();
        return false;
    }

    std::wstring organization;
    std::wstring read_error;
    if (!ReadSignerOrganization(name, organization, read_error)) {
        // A trusted signature with unreadable certificate details is still a usable parent; the
        // client repeats the full check. Keep the reason in the publisher slot for --check.
        publisher = L"(unreadable: " + read_error + L")";
        return true;
    }
    if (!IsPublisherOrganization(organization)) {
        error = L"官方启动器的签名者不是鹰角 / The launcher is not signed by Hypergryph:\n" +
            organization + L"\n" + path.wstring() +
            L"\n请用 --parent 指定正确安装的 Games.exe。 / Point --parent at the installed Games.exe.";
        return false;
    }
    publisher = organization.empty() ? std::wstring(kPublisher) : organization;
    return true;
}

bool IsValidExecutableImage(HANDLE file) {
    if (!file || file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD bytes = 0;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(file, &dos, sizeof(dos), &bytes, nullptr) || bytes != sizeof(dos)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN);
    DWORD nt_signature = 0;
    if (!ReadFile(file, &nt_signature, sizeof(nt_signature), &bytes, nullptr) || bytes != sizeof(nt_signature)) return false;
    return nt_signature == IMAGE_NT_SIGNATURE;
}

bool ResolveToGamesCore(const std::filesystem::path& input, std::filesystem::path& resolved) {
    std::filesystem::path folder = input;
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        if (Equal(input.filename().wstring(), L"Games.exe")) {
            resolved = input;
            return true;
        }
        folder = input.parent_path();
    }
    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_directory(folder, ec)) {
        if (std::filesystem::is_regular_file(folder / L"Games.exe", ec)) candidates.push_back(folder / L"Games.exe");
        for (std::filesystem::directory_iterator cur(folder, ec), end; !ec && cur != end; cur.increment(ec)) {
            const auto games_path = cur->path() / L"Games.exe";
            if (!Version(games_path).empty() && std::filesystem::is_regular_file(games_path, ec)) candidates.push_back(games_path);
        }
    }
    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return Version(a) > Version(b);
        });
        resolved = candidates.front();
        return true;
    }
    if (std::filesystem::is_regular_file(input, ec)) {
        resolved = input;
        return true;
    }
    return false;
}

bool VerifyOfficialLauncher(const std::filesystem::path& input, VerifiedLauncher& result,
    std::wstring& error) {
    std::filesystem::path resolved;
    const auto target = ResolveToGamesCore(input, resolved) ? resolved : input;
    const auto path = std::filesystem::absolute(target).lexically_normal();
    if (!Equal(path.extension().wstring(), L".exe")) {
        error = L"官方父进程必须是可执行程序 (.exe)。 / The official parent must be an executable (.exe).\n" + path.wstring();
        return false;
    }
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = WindowsError(L"无法读取官方启动器 / Cannot read official launcher", GetLastError());
        error += L"\n" + path.wstring();
        return false;
    }
    if (!IsValidExecutableImage(file.get())) {
        error = L"文件不是有效的 Windows PE 可执行文件。 / The file is not a valid Windows PE executable.\n" + path.wstring();
        return false;
    }
    if (!VerifyAuthenticode(path, result.publisher, error)) return false;
    result.executable = path;
    result.file = std::move(file);
    return true;
}

} // namespace

Handle::~Handle() { reset(); }
Handle::Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
Handle& Handle::operator=(Handle&& other) noexcept {
    if (this != &other) { reset(); value_ = std::exchange(other.value_, nullptr); }
    return *this;
}
void Handle::reset(HANDLE value) {
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    value_ = value;
}

std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

bool ParseOptions(int argc, wchar_t* argv[], Options& options, std::wstring& error) {
    options = {};
    bool game_arguments = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (game_arguments) { options.arguments.push_back(option); continue; }
        if (option == L"--") game_arguments = true;
        else if (option == L"--game" && index + 1 < argc && options.game.empty()) options.game = argv[++index];
        else if (option == L"--parent" && index + 1 < argc && options.parent.empty()) options.parent = argv[++index];
        else if (option == L"--home" && index + 1 < argc && options.home.empty()) options.home = argv[++index];
        else if (option == L"--hide-console") options.hide_console = true;
        else if (option == L"--check") options.check = true;
        else if (option == L"--no-parent" || option == L"--direct-launch") options.no_parent = true;
        else { error = L"无效启动参数 / Invalid launch option: " + option; return false; }
    }
    if (options.game.empty() || !Equal(options.game.filename().wstring(), L"Endfield.exe")) {
        error = L"请指定 --game <Endfield.exe>。 / Specify --game <Endfield.exe>.";
        return false;
    }
    options.game = std::filesystem::absolute(options.game).lexically_normal();
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(options.game, filesystem_error)) {
        error = L"找不到游戏文件 / Game executable not found: " + options.game.wstring();
        return false;
    }
    return true;
}

std::wstring WindowsError(const wchar_t* operation, DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = std::wstring(operation) + L" (Win32 " + std::to_wstring(code) + L")";
    if (message) { result += L"\n"; result += message; LocalFree(message); }
    if (code == ERROR_ELEVATION_REQUIRED)
        result += L"\n需要管理员权限，请允许正常的 Windows UAC 提示。 / Administrator privileges are required; allow Windows UAC.";
    return result;
}

void ShowError(const std::wstring& message) {
    std::wcerr << message << L'\n';
    MessageBoxW(nullptr, message.c_str(), L"EFML — 启动失败 / Launch failed", MB_OK | MB_ICONERROR);
}

bool FindOfficialLauncher(const std::filesystem::path& requested, VerifiedLauncher& result,
    std::wstring& error) {
    if (!requested.empty()) return VerifyOfficialLauncher(requested, result, error);
    std::vector<std::filesystem::path> candidates;
    for (const auto& root : InstalledRoots()) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(root / L"Games.exe", filesystem_error)) candidates.push_back(root / L"Games.exe");
        if (std::filesystem::is_regular_file(root / L"Launcher.exe", filesystem_error)) candidates.push_back(root / L"Launcher.exe");
        for (std::filesystem::directory_iterator current(root, filesystem_error), end;
                !filesystem_error && current != end; current.increment(filesystem_error)) {
            const auto games_path = current->path() / L"Games.exe";
            if (!Version(games_path).empty() && std::filesystem::is_regular_file(games_path, filesystem_error)) candidates.push_back(games_path);
            const auto launcher_path = current->path() / L"Launcher.exe";
            if (!Version(launcher_path).empty() && std::filesystem::is_regular_file(launcher_path, filesystem_error)) candidates.push_back(launcher_path);
            filesystem_error.clear();
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return Version(left) > Version(right);
    });
    std::wstring last_error;
    for (const auto& path : candidates) {
        std::wstring candidate_error;
        if (VerifyOfficialLauncher(path, result, candidate_error)) return true;
        if (last_error.empty()) last_error = candidate_error;
    }
    error = L"未找到可用的官方启动器（如 Games.exe / Launcher.exe）。请先安装/启动一次鹰角启动器，"
        L"或在设置中手动指定启动器路径后重试。\n"
        L"No official launcher found. Install the Hypergryph launcher once or set its path.";
    if (!last_error.empty()) error += L"\n\n" + last_error;
    return false;
}

OfficialParent::~OfficialParent() {
    if (!process_.get()) return;
    if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT && !TerminateProcess(process_.get(), 0)) {
        std::wcerr << WindowsError(L"清理自建启动器父进程 / Clean up owned launcher parent", GetLastError()) << L'\n';
        return;
    }
    WaitForSingleObject(process_.get(), 5000);
}

bool OfficialParent::Start(const VerifiedLauncher& launcher, std::wstring& error) {
    if (process_.get() || !launcher.file.get() || launcher.file.get() == INVALID_HANDLE_VALUE) {
        error = L"Invalid verified launcher state.";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    auto command = QuoteArgument(launcher.executable.wstring());
    if (!CreateProcessW(launcher.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, launcher.executable.parent_path().c_str(),
            &startup, &process)) {
        error = WindowsError(L"创建官方启动器父进程 / Create official launcher parent", GetLastError());
        return false;
    }
    process_.reset(process.hProcess);
    pid_ = process.dwProcessId;
    CloseHandle(process.hThread);
    return true;
}

bool OfficialParent::CreateGame(const std::filesystem::path& game,
    const std::vector<std::wstring>& arguments, DWORD creation_flags,
    PROCESS_INFORMATION& process, std::wstring& error) const {
    if (!process_.get()) {
        error = L"The official parent process is not running.";
        return false;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) { error = WindowsError(L"Prepare parent attributes", GetLastError()); return false; }
    std::vector<unsigned char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        error = WindowsError(L"Initialize parent attributes", GetLastError());
        return false;
    }
    struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST list; ~Cleanup() { DeleteProcThreadAttributeList(list); } } cleanup{attributes};
    HANDLE parent = process_.get();
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr)) {
        error = WindowsError(L"Set official parent", GetLastError());
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    auto command = QuoteArgument(game.wstring());
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE,
            creation_flags | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
            nullptr, game.parent_path().c_str(), &startup.StartupInfo, &process)) {
        error = WindowsError(L"创建游戏进程 / Create game process", GetLastError());
        return false;
    }
    return true;
}

bool CreateGameProcess(const std::filesystem::path& game,
    const std::vector<std::wstring>& arguments, DWORD creation_flags,
    PROCESS_INFORMATION& process, std::wstring& error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    auto command = QuoteArgument(game.wstring());
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE,
            creation_flags, nullptr, game.parent_path().c_str(), &startup, &process)) {
        error = WindowsError(L"创建游戏进程 / Create game process", GetLastError());
        return false;
    }
    return true;
}

bool WaitForGame(HANDLE process, DWORD& exit_code, std::wstring& error) {
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        error = WindowsError(L"等待游戏退出 / Wait for game exit", GetLastError());
        return false;
    }
    if (!GetExitCodeProcess(process, &exit_code)) {
        error = WindowsError(L"读取游戏退出状态 / Read game exit status", GetLastError());
        return false;
    }
    return true;
}

} // namespace EFML::Loader::Launch
