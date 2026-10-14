// Module host tests without the game: index handling, all entry names, dependencies, retries,
// RPC, services, events and the game-thread queue.
// Usage: efml-tests.exe <example.echo.dll> <test.legacy.dll>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include "../host/game_thread.h"
#include "../host/logging.h"
#include "../host/module_host.h"
#include "../vendor/nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

using J = nlohmann::json;
namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const std::string& name) {
    ++checks;
    std::cout << (condition ? "  ok   " : "  FAIL ") << name << "\n";
    if (!condition) ++failures;
}

J ReadJson(const fs::path& path) {
    std::ifstream in(path);
    return J::parse(in);
}

void WriteJson(const fs::path& path, const J& value) {
    const auto temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << value.dump(2);
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    // The host compares write times; make sure consecutive writes differ.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// Minimal HTTP/1.1 client for the loopback RPC.
std::pair<int, J> Request(uint16_t port, const std::string& token, const std::string& method,
    const std::string& path, const J& body = nullptr) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(s);
        return {0, nullptr};
    }
    const std::string payload = body.is_null() ? "" : body.dump();
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
    send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buffer[4096];
    int count = 0;
    while ((count = recv(s, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, static_cast<size_t>(count));
    closesocket(s);
    const auto split = response.find("\r\n\r\n");
    if (split == std::string::npos) return {0, nullptr};
    const int code = std::stoi(response.substr(9, 3));
    return {code, J::parse(response.substr(split + 4), nullptr, false)};
}

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return condition();
}

J ModuleStatus(const J& status, const std::string& id) {
    for (const auto& module : status.value("modules", J::array())) {
        if (module.value("id", "") == id) return module;
    }
    return J::object();
}

fs::path Package(const fs::path& root, const std::string& generation, const fs::path& dll, const J& manifest) {
    const auto directory = root / "packages" / generation;
    fs::create_directories(directory / "native" / "windows-x64");
    fs::copy_file(dll, directory / "native" / "windows-x64" / dll.filename(), fs::copy_options::overwrite_existing);
    std::ofstream(directory / "module.json") << manifest.dump(2);
    return directory;
}

void TestGameThread() {
    std::cout << "game thread\n";
    const auto root = fs::temp_directory_path() / ("efml-gt-" + std::to_string(GetCurrentProcessId()));
    EFML::Host::Logger logger;
    logger.Initialize(root);
    EFML::Host::GameThread game(logger);
    static int tasks = 0, frames = 0, ready = 0;
    game.on_ready = [] { ++ready; };
    game.Post("a", [](void*) { ++tasks; }, nullptr);
    uint64_t handle = 0;
    game.AddFrame("a", [](void*, uint64_t) { ++frames; }, nullptr, &handle);
    game.AddFrame("b", [](void*, uint64_t) { ++frames; }, nullptr, nullptr);
    Check(!game.Ready() && !game.IsGameThread(), "not ready before the first frame");
    game.Tick(0);
    Check(game.Ready() && ready == 1 && game.IsGameThread(), "first frame marks the game thread");
    Check(tasks == 1 && frames == 2, "tasks run once, frame callbacks every frame");
    game.Tick(1);   // Without Time.frameCount only the first source counts.
    Check(frames == 2 && game.Frames() == 1, "a second frame source is ignored");
    game.Tick(0);
    Check(tasks == 1 && frames == 4, "frame callbacks repeat, tasks do not");
    Check(game.RemoveFrame("b", handle) == BE_Result_NotFound, "an owner cannot remove another owner's callback");
    game.RemoveOwner("a");
    game.Tick(0);
    Check(frames == 5, "removing an owner stops its callbacks");
    Check(game.Post("a", nullptr, nullptr) == BE_Result_InvalidArgument, "null task rejected");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: efml-tests <example.echo.dll> <test.legacy.dll>\n";
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);
    std::cout << std::unitbuf;
    try {
        TestGameThread();
    } catch (const std::exception& error) {
        std::cout << "exception: " << error.what() << "\n";
        return 1;
    }

    std::cout << "module host\n";
    const auto root = fs::temp_directory_path() / ("efml-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    const auto index_path = root / "modules" / "index.json";
    std::string error;
    Check(EFML::Host::ModuleHost::EnsureIndex(index_path, error), "index created when missing");
    auto index = ReadJson(index_path);
    const uint16_t port = index.at("port").get<uint16_t>();
    const std::string token = index.at("token").get<std::string>();
    Check(port >= 1024 && token.size() == 64, "index has a port and a 64-hex token");

    const auto modules_root = root / "modules";
    const auto echo_dir = Package(modules_root, "11111111-1111-4111-8111-111111111111", argv[1],
        {{"format", 1}, {"abi", 1}, {"id", "example.echo"}, {"name", "Echo"}, {"version", "1.0.0"},
         {"libraries", {{"windows-x64", "native/windows-x64/example.echo.dll"}}}});
    const auto legacy_dir = Package(modules_root, "22222222-2222-4222-8222-222222222222", argv[2],
        {{"format", 1}, {"abi", 1}, {"id", "test.legacy"}, {"name", "Legacy"}, {"version", "0.1.0"},
         {"dependencies", {"example.echo"}},
         {"libraries", {{"windows-x64", "native/windows-x64/" + fs::path(argv[2]).filename().string()}}}});
    // Legacy first in the index: the dependency must still start echo first.
    index["modules"] = J::array({
        {{"id", "test.legacy"}, {"directory", legacy_dir.string()}, {"generation", "22222222-2222-4222-8222-222222222222"},
         {"enabled", true}, {"configuration", J::object()}},
        {{"id", "example.echo"}, {"directory", echo_dir.string()}, {"generation", "11111111-1111-4111-8111-111111111111"},
         {"enabled", true}, {"configuration", {{"label", "first"}}}},
        {{"id", "efml.reserved"}, {"directory", echo_dir.string()}, {"enabled", false}}});
    WriteJson(index_path, index);

    EFML::Host::Logger logger;
    logger.Initialize(root / "logs");
    EFML::Host::GameThread game(logger);
    EFML::Host::ModuleHost host;
    EFML::Host::ModuleHost::Options options;
    options.index = index_path;
    options.data_root = root.string();
    options.log = [&](const std::string& source, const std::string& message) { logger.Write(source, message); };
    options.game_thread = &game;
    Check(host.Start(options), "host starts and listens");

    // A reserved id makes the whole index invalid, like Better-Endfield.
    auto [code, status] = Request(port, token, "GET", "/status");
    Check(code == 200 && status.value("connected", false), "status answers");
    Check(ModuleStatus(status, "example.echo").empty(), "reserved efml.* id rejects the index");
    index["modules"].erase(2);
    WriteJson(index_path, index);
    Check(WaitFor([&] { return ModuleStatus(Request(port, token, "GET", "/status").second, "example.echo")
        .value("status", "") == "waiting_runtime"; }), "modules wait for the IL2CPP runtime");

    Check(Request(port, "wrong-token-wrong-token-wrong-token", "GET", "/status").first == 401, "wrong token gets 401");

    static BE_HostApiV1 fake_runtime{};
    fake_runtime.abi_version = 1;
    host.SetRuntime(&fake_runtime, [] { return reinterpret_cast<void*>(1); });
    Check(WaitFor([&] { return ModuleStatus(Request(port, token, "GET", "/status").second, "example.echo")
        .value("status", "") == "ready"; }), "echo starts once the runtime is ready");
    status = Request(port, token, "GET", "/status").second;
    const auto echo = ModuleStatus(status, "example.echo");
    Check(echo.value("entry", "") == "EFML_GetModuleV1", "EFML entry preferred");
    Check(status["host"].value("worker_attached", false), "worker attached to IL2CPP");
    bool service_listed = false;
    for (const auto& service : status["host"]["services"]) service_listed |= service.value("name", "") == "example.echo.counter";
    Check(service_listed, "service registered");

    Check(WaitFor([&] { return ModuleStatus(Request(port, token, "GET", "/status").second, "test.legacy")
        .value("status", "") == "ready"; }), "legacy module retried after NotReady and started");
    const auto legacy = ModuleStatus(Request(port, token, "GET", "/status").second, "test.legacy");
    Check(legacy.value("entry", "") == "BetterEndfield_GetThirdPartyModuleV1", "Better-Endfield entry name loads");

    Request(port, token, "POST", "/poll", {{"module_id", "example.echo"}});
    auto sent = Request(port, token, "POST", "/send", {{"module_id", "example.echo"}, {"request_id", "r1"}, {"body", {{"x", 1}}}});
    Check(sent.first == 200 && sent.second.value("accepted", false), "send accepted");
    J reply;
    Check(WaitFor([&] {
        const auto polled = Request(port, token, "POST", "/poll", {{"module_id", "example.echo"}});
        if (getenv("EFML_TEST_VERBOSE")) std::cout << polled.first << " " << polled.second.dump() << "\n";
        for (const auto& message : polled.second.value("messages", J::array())) {
            if (message.value("kind", "") == "reply" && message.value("request_id", "") == "r1") reply = message;
        }
        return !reply.is_null();
    }), "reply delivered");
    Check(reply.is_object() && reply["body"].value("counter", 0) == 1 && reply["body"].value("efml", false) &&
        reply["body"]["configuration"].value("label", "") == "first", "reply carries counter, EFML flag and configuration");

    auto legacy_reply = Request(port, token, "POST", "/send", {{"module_id", "test.legacy"}, {"request_id", "l1"}, {"body", J::object()}});
    J legacy_body;
    Check(legacy_reply.first == 200 && WaitFor([&] {
        const auto polled = Request(port, token, "POST", "/poll", {{"module_id", "test.legacy"}});
        if (getenv("EFML_TEST_VERBOSE")) std::cout << polled.first << " " << polled.second.dump() << "\n";
        for (const auto& message : polled.second.value("messages", J::array())) {
            if (message.value("request_id", "") == "l1") legacy_body = message["body"];
        }
        return !legacy_body.is_null();
    }) && legacy_body.value("service", false), "a module finds another module's service");

    auto configured = Request(port, token, "POST", "/configure", {{"module_id", "example.echo"}, {"configuration", {{"label", "second"}}}});
    Check(configured.first == 200 && configured.second.value("accepted", false), "configure applied");
    Check(host.Configuration("example.echo").find("second") != std::string::npos, "configuration visible to the runtime API");

    host.PublishEvent("{\"type\":\"test_event\"}");
    bool host_event = false;
    Check(WaitFor([&] {
        const auto polled = Request(port, token, "POST", "/poll", {{"module_id", "example.echo"}});
        if (getenv("EFML_TEST_VERBOSE")) std::cout << polled.first << " " << polled.second.dump() << "\n";
        for (const auto& message : polled.second.value("messages", J::array())) {
            host_event |= message["body"].value("type", "") == "host_event";
        }
        return host_event;
    }), "host events reach subscribers");

    auto guarded = Request(port, token, "POST", "/configure",
        {{"module_id", "example.echo"}, {"configuration", {{"label", "second"}, {"shutdown_wait", 3}}}});
    Check(guarded.first == 200 && guarded.second.value("accepted", false), "shutdown-barrier fixture configured");
    index["modules"][1]["configuration"] = {{"label", "second"}, {"shutdown_wait", 3}};
    index["modules"][1]["enabled"] = false;
    WriteJson(index_path, index);
    Check(WaitFor([&] {
        const auto now = Request(port, token, "GET", "/status").second;
        return ModuleStatus(now, "example.echo").value("status", "") == "restoring";
    }), "shutdown barrier defers disable before callbacks are withdrawn");
    Check(Request(port, token, "POST", "/send", {{"module_id", "example.echo"}, {"request_id", "pending"}, {"body", 1}}).first == 200,
        "module stays active while native restoration is pending");
    Check(WaitFor([&] {
        const auto now = Request(port, token, "GET", "/status").second;
        return ModuleStatus(now, "example.echo").value("status", "") == "disabled" &&
            ModuleStatus(now, "test.legacy").value("status", "") == "waiting_dependencies";
    }), "disabling a provider stops its dependents");
    status = Request(port, token, "GET", "/status").second;
    Check(status["host"]["services"].empty(), "services withdrawn with their provider");
    Check(Request(port, token, "POST", "/send", {{"module_id", "example.echo"}, {"request_id", "r2"}, {"body", 1}}).first == 400,
        "inactive module rejects messages");

    index["modules"][1]["enabled"] = true;
    WriteJson(index_path, index);
    Check(WaitFor([&] {
        const auto now = Request(port, token, "GET", "/status").second;
        return ModuleStatus(now, "example.echo").value("status", "") == "ready" &&
            ModuleStatus(now, "test.legacy").value("status", "") == "ready";
    }), "re-enabling restarts provider and dependent");

    index["port"] = port == 65535 ? 65534 : port + 1;
    WriteJson(index_path, index);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Check(Request(port, token, "GET", "/status").first == 200, "port change ignored until restart");

    host.Stop();
    std::error_code ignored;   // Loaded module DLLs stay locked until the process exits.
    fs::remove_all(root, ignored);
    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
