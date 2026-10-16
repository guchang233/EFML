// efml.exe: command-line manager for the EFML runtime. Works on the same files as the GUI manager:
// %LOCALAPPDATA%\EFML\{loader.ini, runtime\<version>\, modules\index.json, modules\packages\, logs\}.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "../host/version.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using J = nlohmann::json;

namespace {

const wchar_t* const kRuntimeFiles[] = {L"EFML.Host.dll", L"EFML.Bootstrap.dll", L"EFML.Injector.exe", L"xinput1_4.dll"};

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring Env(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size == 0 || size >= buffer.size() ? std::wstring() : std::wstring(buffer.data(), size);
}

fs::path DataRoot() {
    const auto home = Env(L"EFML_HOME");
    return home.empty() ? fs::path(Env(L"LOCALAPPDATA")) / L"EFML" : fs::path(home);
}
fs::path IndexPath() { return DataRoot() / L"modules" / L"index.json"; }
fs::path LoaderIni() { return DataRoot() / L"loader.ini"; }

std::wstring IniRead(const wchar_t* section, const wchar_t* key) {
    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(section, key, L"", value.data(), static_cast<DWORD>(value.size()), LoaderIni().c_str());
    return value.data();
}
void IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    fs::create_directories(DataRoot());
    WritePrivateProfileStringW(section, key, value.c_str(), LoaderIni().c_str());
}

[[noreturn]] void Fail(const std::string& message, int code = 1) {
    std::cerr << "efml: " << message << "\n";
    std::exit(code);
}

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned char c : data) {
        text.push_back(digits[c >> 4]);
        text.push_back(digits[c & 15]);
    }
    return text;
}

std::string NewGuid() {
    auto hex = RandomHex(16);
    hex[12] = '4';
    hex[16] = "89ab"[std::stoi(hex.substr(16, 1), nullptr, 16) & 3];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

uint16_t FreePort() {
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    uint16_t port = 0;
    if (bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
        port = ntohs(address.sin_port);
    }
    closesocket(probe);
    return port;
}

J LoadIndex() {
    std::ifstream in(IndexPath());
    if (!in) return {{"schema", 1}, {"port", FreePort()}, {"token", RandomHex(32)}, {"modules", J::array()}};
    J index = J::parse(in, nullptr, false);
    if (index.is_discarded() || index.value("schema", 0) != 1) Fail("modules\\index.json is not a valid index");
    return index;
}

void SaveIndex(const J& index) {
    fs::create_directories(IndexPath().parent_path());
    const auto temp = IndexPath().wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << index.dump(2);
    }
    if (!MoveFileExW(temp.c_str(), IndexPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Fail("could not write modules\\index.json");
    }
}

J* FindModule(J& index, const std::string& id) {
    for (auto& module : index["modules"]) {
        if (module.value("id", "") == id) return &module;
    }
    return nullptr;
}

J ReadManifest(const fs::path& directory) {
    std::ifstream in(directory / L"module.json");
    if (!in) Fail("module.json not found in " + Utf8(directory.wstring()));
    J manifest = J::parse(in, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) Fail("module.json is not valid JSON");
    if (manifest.value("format", 0) != 1 || manifest.value("abi", 0) != 1) Fail("module.json must have format 1 and abi 1");
    const auto id = manifest.value("id", "");
    if (id.empty() || id.size() > 96) Fail("module.json has no valid id");
    if (!manifest.contains("libraries") && !manifest.contains("ui")) Fail("module.json declares neither libraries nor ui");
    return manifest;
}

// ---- RPC ----------------------------------------------------------------------------------

std::optional<J> Rpc(const J& index, const std::string& method, const std::string& path, const J& body = nullptr) {
    WSADATA ws{};
    WSAStartup(MAKEWORD(2, 2), &ws);
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(index.value("port", 0));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(s);
        return std::nullopt;
    }
    const std::string payload = body.is_null() ? "" : body.dump();
    const std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
        index.value("token", "") + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload;
    send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buffer[8192];
    int count = 0;
    while ((count = recv(s, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, static_cast<size_t>(count));
    closesocket(s);
    const auto split = response.find("\r\n\r\n");
    if (split == std::string::npos) return std::nullopt;
    J parsed = J::parse(response.substr(split + 4), nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

// ---- Game discovery -------------------------------------------------------------------------

std::vector<fs::path> RegistryCandidates() {
    std::vector<fs::path> found;
    const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    const REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            HKEY uninstall = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &uninstall) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0;; ++i) {
                DWORD length = 256;
                if (RegEnumKeyExW(uninstall, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                auto read = [&](const wchar_t* value) {
                    wchar_t data[2048]{};
                    DWORD bytes = sizeof(data);
                    return RegGetValueW(uninstall, name, value, RRF_RT_REG_SZ, nullptr, data, &bytes) == ERROR_SUCCESS ? std::wstring(data) : std::wstring();
                };
                const auto display = read(L"DisplayName");
                if (display.find(L"Endfield") == std::wstring::npos && display.find(L"终末地") == std::wstring::npos) continue;
                for (auto location : {read(L"InstallLocation"), read(L"InstallPath")}) {
                    if (location.empty()) continue;
                    location.erase(std::remove(location.begin(), location.end(), L'"'), location.end());
                    found.push_back(fs::path(location) / L"Endfield.exe");
                }
            }
            RegCloseKey(uninstall);
        }
    }
    return found;
}

fs::path FindGame(const std::string& argument) {
    std::vector<fs::path> candidates;
    if (!argument.empty()) {
        fs::path given = Wide(argument);
        candidates.push_back(fs::is_directory(given) ? given / L"Endfield.exe" : given);
    }
    if (const auto saved = IniRead(L"Game", L"path"); !saved.empty()) candidates.push_back(saved);
    for (const auto& c : RegistryCandidates()) candidates.push_back(c);
    for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
        const std::wstring root = std::wstring(1, drive) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* folder : {L"Arknights Endfield", L"Endfield Game", L"Games\\Arknights Endfield", L"Program Files\\Hypergryph Launcher\\games\\Endfield Game"}) {
            candidates.push_back(fs::path(root) / folder / L"Endfield.exe");
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (_wcsicmp(candidate.filename().c_str(), L"Endfield.exe") == 0 && fs::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

bool IsEfmlProxy(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data.find("EFML-XInput-Proxy") != std::string::npos;
}

fs::path InstalledRuntime() {
    const fs::path host = IniRead(L"Loader", L"host");
    return host.empty() ? fs::path() : host.parent_path();
}

// ---- Commands -------------------------------------------------------------------------------

int Setup(const std::string& from) {
    fs::path source = from.empty() ? fs::path() : fs::path(Wide(from));
    if (source.empty()) {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        source = fs::path(self).parent_path();
    }
    for (const auto* file : kRuntimeFiles) {
        if (!fs::is_regular_file(source / file)) Fail("runtime file missing: " + Utf8((source / file).wstring()));
    }
    const auto target = DataRoot() / L"runtime" / EFML_VERSION_W;
    std::error_code error;
    fs::create_directories(target, error);
    for (const auto* file : kRuntimeFiles) {
        if (fs::equivalent(source / file, target / file, error)) continue;
        // A running game keeps the Host locked; the same version is then already in place.
        if (!fs::copy_file(source / file, target / file, fs::copy_options::overwrite_existing, error) && !fs::exists(target / file)) {
            Fail("could not copy " + Utf8(file) + ": " + error.message());
        }
    }
    IniWrite(L"Loader", L"host", (target / L"EFML.Host.dll").wstring());
    IniWrite(L"Loader", L"load_host", L"true");
    SaveIndex(LoadIndex());
    std::cout << "EFML " << EFML_VERSION << " runtime installed in " << Utf8(target.wstring()) << "\n";
    return 0;
}

int Add(const std::string& path, bool dev) {
    fs::path source = Wide(path);
    J index = LoadIndex();
    fs::path directory;
    std::string generation;
    if (fs::is_directory(source) && dev) {
        // Development: register the folder in place. A rebuilt DLL loads after a game restart.
        directory = fs::absolute(source);
        generation = "dev-" + NewGuid();
    } else {
        generation = NewGuid();
        directory = DataRoot() / L"modules" / L"packages" / Wide(generation);
        fs::create_directories(directory);
        if (fs::is_directory(source)) {
            fs::copy(source, directory, fs::copy_options::recursive);
        } else {
            const std::wstring command = L"tar.exe -xf \"" + fs::absolute(source).wstring() + L"\" -C \"" + directory.wstring() + L"\"";
            if (_wsystem(command.c_str()) != 0) {
                fs::remove_all(directory);
                Fail("could not extract " + path);
            }
        }
    }
    J manifest;
    try {
        manifest = ReadManifest(directory);
    } catch (...) {
        if (!dev) fs::remove_all(directory);
        throw;
    }
    const auto id = manifest.value("id", "");
    J* existing = FindModule(index, id);
    J record = {{"id", id}, {"directory", Utf8(directory.wstring())}, {"generation", generation},
        {"enabled", existing ? existing->value("enabled", false) : true},
        {"configuration", existing ? existing->value("configuration", J::object()) : manifest.value("default_configuration", J::object())}};
    if (existing) {
        index["retired_directories"].push_back(existing->value("directory", ""));
        *existing = record;
    } else {
        index["modules"].push_back(record);
    }
    SaveIndex(index);
    std::cout << (existing ? "updated " : "added ") << id << " " << manifest.value("version", "") << (dev ? " (dev, in place)" : "") << "\n";
    return 0;
}

int SetEnabled(const std::string& id, bool enabled) {
    J index = LoadIndex();
    J* module = FindModule(index, id);
    if (!module) Fail("no module " + id);
    (*module)["enabled"] = enabled;
    SaveIndex(index);
    std::cout << id << (enabled ? " enabled\n" : " disabled\n");
    return 0;
}

int Remove(const std::string& id) {
    J index = LoadIndex();
    auto& modules = index["modules"];
    const auto it = std::find_if(modules.begin(), modules.end(), [&](const J& m) { return m.value("id", "") == id; });
    if (it == modules.end()) Fail("no module " + id);
    // Package folders are retired, not deleted: a running game may still have the DLL loaded.
    if (!it->value("generation", "").starts_with("dev-")) index["retired_directories"].push_back(it->value("directory", ""));
    modules.erase(it);
    SaveIndex(index);
    std::cout << "removed " << id << " (takes full effect after the game restarts)\n";
    return 0;
}

int Configure(const std::string& id, const std::string& json) {
    J config = J::parse(json, nullptr, false);
    if (!config.is_object()) Fail("configuration must be a JSON object");
    J index = LoadIndex();
    J* module = FindModule(index, id);
    if (!module) Fail("no module " + id);
    (*module)["configuration"] = config;
    SaveIndex(index);
    std::cout << "configuration saved for " << id << "\n";
    return 0;
}

int Status() {
    const J index = LoadIndex();
    std::cout << "EFML " << EFML_VERSION << "  data " << Utf8(DataRoot().wstring()) << "\n";
    const auto runtime = InstalledRuntime();
    std::cout << "runtime   " << (runtime.empty() ? "not installed (run: efml setup)" : Utf8(runtime.wstring())) << "\n";
    const auto game = FindGame("");
    std::cout << "game      " << (game.empty() ? "not found (efml proxy install --game <path>)" : Utf8(game.wstring())) << "\n";
    if (!game.empty()) {
        const auto proxy = game.parent_path() / L"xinput1_4.dll";
        std::cout << "proxy     " << (!fs::exists(proxy) ? "not installed" : IsEfmlProxy(proxy) ? "EFML proxy installed" : "foreign xinput1_4.dll (another loader)") << "\n";
    }
    const auto live = Rpc(index, "GET", "/status");
    if (live) {
        const auto& host = (*live)["host"];
        std::cout << "host      running in pid " << host.value("pid", 0) << ", runtime " << (host.value("runtime_ready", false) ? "ready" : "waiting");
        if (host.contains("game_thread")) std::cout << ", game thread " << (host["game_thread"].value("ready", false) ? "ready (" + host["game_thread"].value("source", "") + ", " + std::to_string(host["game_thread"].value("frames", 0ull)) + " frames)" : "waiting");
        std::cout << "\n";
    } else {
        std::cout << "host      not running (game closed, or loaded without EFML)\n";
    }
    std::cout << "\nmodules\n";
    for (const auto& module : index["modules"]) {
        const auto id = module.value("id", "");
        std::string state = module.value("enabled", false) ? "enabled" : "disabled";
        std::string error;
        if (live) {
            for (const auto& m : (*live)["modules"]) {
                if (m.value("id", "") != id) continue;
                state = m.value("status", state);
                error = m.value("error", "");
                if (m.value("restart_required", false)) state += ", restart required";
            }
        }
        std::printf("  %-28s %-24s %s\n", id.c_str(), state.c_str(), error.c_str());
    }
    if (index["modules"].empty()) std::cout << "  (none; add one with: efml add <module.zip | folder>)\n";
    return 0;
}

int Logs(const std::string& id) {
    const J index = LoadIndex();
    const auto live = Rpc(index, "GET", "/status");
    if (!live) {
        std::ifstream in(DataRoot() / L"logs" / L"EFML.log");
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(in, line)) {
            if (id.empty() || line.find("[" + id + "]") != std::string::npos) lines.push_back(line);
        }
        for (size_t i = lines.size() > 40 ? lines.size() - 40 : 0; i < lines.size(); ++i) std::cout << lines[i] << "\n";
        return 0;
    }
    for (const auto& m : (*live)["modules"]) {
        if (!id.empty() && m.value("id", "") != id) continue;
        for (const auto& line : m["logs"]) std::cout << "[" << m.value("id", "") << "] " << line.get<std::string>() << "\n";
    }
    return 0;
}

int Send(const std::string& id, const std::string& json) {
    const J index = LoadIndex();
    J body = J::parse(json, nullptr, false);
    if (body.is_discarded()) Fail("message must be JSON");
    const auto request = "cli-" + RandomHex(4);
    const auto sent = Rpc(index, "POST", "/send", {{"module_id", id}, {"request_id", request}, {"body", body}});
    if (!sent) Fail("the game is not running with EFML");
    if (!sent->value("accepted", false)) Fail(sent->value("error", "message rejected"));
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto polled = Rpc(index, "POST", "/poll", {{"module_id", id}});
        if (polled) {
            for (const auto& message : (*polled)["messages"]) {
                if (message.value("request_id", "") == request) {
                    std::cout << message["body"].dump(2) << "\n";
                    return message.value("result", 0) == 0 ? 0 : 1;
                }
            }
        }
        Sleep(100);
    }
    Fail("no reply within 5 seconds");
}

int Proxy(bool install, const std::string& game_argument) {
    const auto game = FindGame(game_argument);
    if (game.empty()) Fail("Endfield.exe not found; pass --game <path>");
    const auto proxy = game.parent_path() / L"xinput1_4.dll";
    std::error_code error;
    if (install) {
        const auto runtime = InstalledRuntime();
        if (runtime.empty()) Fail("install the runtime first: efml setup");
        if (fs::exists(proxy) && !IsEfmlProxy(proxy)) Fail("a foreign xinput1_4.dll is in the game folder (another loader); remove it first");
        if (!fs::copy_file(runtime / L"xinput1_4.dll", proxy, fs::copy_options::overwrite_existing, error)) Fail("could not copy the proxy: " + error.message() + " (close the game, or run as administrator)");
        IniWrite(L"Game", L"path", game.wstring());
        std::cout << "proxy installed: " << Utf8(proxy.wstring()) << "\nThe game now loads EFML however it is started.\n";
    } else {
        if (!fs::exists(proxy)) {
            std::cout << "no proxy installed\n";
            return 0;
        }
        if (!IsEfmlProxy(proxy)) Fail("xinput1_4.dll in the game folder is not EFML's; leaving it alone");
        if (!fs::remove(proxy, error)) Fail("could not remove the proxy: " + error.message() + " (close the game first)");
        std::cout << "proxy removed\n";
    }
    return 0;
}

// EFML does not start the game: Endfield 1.6.3 only accepts a start from the official launcher.
// This makes sure the proxy is in place and tells the user how to start the game.
int Launch(const std::string& game_argument) {
    const auto game = FindGame(game_argument);
    if (game.empty()) Fail("Endfield.exe not found; pass --game <path>");
    const auto proxy = game.parent_path() / L"xinput1_4.dll";
    if (fs::exists(proxy) && !IsEfmlProxy(proxy)) Fail("another loader's xinput1_4.dll is in the game folder; remove it with its own tool first");
    if (!(fs::exists(proxy) && IsEfmlProxy(proxy))) Proxy(true, Utf8(game.wstring()));
    IniWrite(L"Game", L"path", game.wstring());
    std::cout << "EFML is ready. It does not start the game itself.\n"
                 "Start Endfield from the official Hypergryph launcher (click Start Game);\n"
                 "the proxy then loads EFML and the enabled modules. Run `efml status` to watch them.\n";
    return 0;
}

void Usage() {
    std::cout <<
        "EFML " EFML_VERSION " - Endfield Mod Loader\n\n"
        "  efml setup [runtime-folder]       install the runtime (default: files next to efml.exe)\n"
        "  efml status                       runtime, game, proxy, live host and module states\n"
        "  efml add <module.zip|folder>      install or update a module\n"
        "  efml add --dev <folder>           register a module folder in place (development)\n"
        "  efml enable|disable|remove <id>\n"
        "  efml config <id> <json>           replace a module's configuration\n"
        "  efml send <id> <json>             send a message to a running module and print the reply\n"
        "  efml logs [id]                    recent module logs\n"
        "  efml launch [--game <path>]       install the proxy if needed and explain how to start the game\n"
        "                                    (EFML never starts the game; use the official launcher)\n"
        "  efml proxy install|remove [--game <path>]\n"
        "                                    xinput1_4.dll proxy: EFML loads however the game starts\n\n"
        "Data: %LOCALAPPDATA%\\EFML (override with EFML_HOME).\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA ws{};
    WSAStartup(MAKEWORD(2, 2), &ws);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(Utf8(argv[i]));
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        Usage();
        return 0;
    }
    auto option = [&](const std::string& name) {
        for (size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == name) return args[i + 1];
        }
        return std::string();
    };
    const auto& command = args[0];
    try {
        if (command == "setup") return Setup(args.size() > 1 ? args[1] : "");
        if (command == "status") return Status();
        if (command == "add" && args.size() >= 3 && args[1] == "--dev") return Add(args[2], true);
        if (command == "add" && args.size() >= 2) return Add(args[1], false);
        if (command == "enable" && args.size() >= 2) return SetEnabled(args[1], true);
        if (command == "disable" && args.size() >= 2) return SetEnabled(args[1], false);
        if (command == "remove" && args.size() >= 2) return Remove(args[1]);
        if (command == "config" && args.size() >= 3) return Configure(args[1], args[2]);
        if (command == "send" && args.size() >= 3) return Send(args[1], args[2]);
        if (command == "logs") return Logs(args.size() > 1 ? args[1] : "");
        if (command == "proxy" && args.size() >= 2 && (args[1] == "install" || args[1] == "remove")) {
            return Proxy(args[1] == "install", option("--game"));
        }
        if (command == "launch") return Launch(option("--game"));
    } catch (const std::exception& error) {
        Fail(error.what());
    }
    Usage();
    return 2;
}
