// Crash reporter tests without the game: a real first-chance access violation is caught by
// __except after the vectored handler recorded it.
// Usage: efml-crash-tests.exe
#include <Windows.h>

#include "../host/crash_reporter.h"
#include "../cli/crash_log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using EFML::Host::CrashReporter;

namespace {

int failures = 0;

void Check(bool condition, const std::string& name) {
    std::cout << (condition ? "  ok   " : "  FAIL ") << name << "\n";
    if (!condition) ++failures;
}

// Not a leaf (it calls out first), so it has unwind data like real game methods.
__declspec(noinline) void WriteThrough(volatile int* pointer) {
    char value[16];
    GetEnvironmentVariableA("EFML_CRASH_TEST_UNUSED", value, sizeof(value));
    *pointer = 7;
}

// No C++ objects here: __try needs a frame without unwinding.
bool FaultOnce(volatile int* pointer) {
    __try {
        WriteThrough(pointer);
        return false;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("efml-crash-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);

    auto& reporter = CrashReporter::Instance();
    reporter.Install(root, "efml-crash-tests.exe");
    Check(fs::exists(root / "crash.log"), "log created");

    const int slot = reporter.RegisterModule("test.self", GetModuleHandleW(nullptr));
    Check(slot == 0, "module registered");
    Check(reporter.RegisterModule("test.self", GetModuleHandleW(nullptr)) == slot, "same image keeps its slot");
    reporter.Mark(slot, "applying world", "peer 2 / chen_attack3");
    reporter.AddLabel(reinterpret_cast<const void*>(&WriteThrough), "Test::WriteThrough(1 args)");

    Check(FaultOnce(nullptr), "fault reached the test's own handler");
    Check(reporter.Recorded() == 1, "one report written");
    Check(FaultOnce(nullptr), "same fault again");
    Check(reporter.Recorded() == 1, "same address reported once");

    reporter.Mark(slot, nullptr, nullptr);
    reporter.Uninstall();

    const auto log = ReadAll(root / "crash.log");
    Check(log.find("process start") != std::string::npos, "session header");
    Check(log.find("first-chance") != std::string::npos, "first-chance noted");
    Check(log.find("access violation 0xC0000005, write 0x0") != std::string::npos, "code and access");
    Check(log.find("test.self: applying world peer 2 / chen_attack3") != std::string::npos, "module step");
    Check(log.find("modules on this stack: test.self") != std::string::npos, "module on stack");
    Check(log.find("Test::WriteThrough(1 args) +0x") != std::string::npos, "label of faulting function");
    Check(log.find("[module test.self]") != std::string::npos, "frame attributed to module");
    Check(log.find("#01 ") != std::string::npos, "stack unwound past the fault");

    // The CLI parser (efml crash --json, also used by the manager) reads the same report back.
    const auto parsed = EFML::CrashLog::Parse(log);
    const EFML::CrashLog::J* parsedReport = nullptr;
    for (const auto& item : parsed) if (item.contains("exception")) { parsedReport = &item; break; }
    Check(parsedReport != nullptr, "parser found the report");
    if (parsedReport) {
        const auto& r = *parsedReport;
        Check(r.value("exception", "") == "access violation" && r.value("code", "") == "0xC0000005", "parsed exception");
        Check(r.contains("access") && r["access"].value("kind", "") == "write", "parsed access");
        Check(!r.value("fatal", true), "parsed first-chance");
        Check(r["location"].value("method", "") == "Test::WriteThrough(1 args)", "parsed faulting method");
        Check(r["modules_on_stack"].size() == 1 && r["modules_on_stack"][0] == "test.self", "parsed modules on stack");
        Check(r["steps"].size() == 1 && r["steps"][0].value("step", "") == "applying world peer 2 / chen_attack3", "parsed step");
        Check(r["stack"].size() >= 2 && r["stack"][0].value("index", -1) == 0, "parsed stack frames");
        Check(r["registers"].contains("rip"), "parsed registers");
    }

    // A synthetic stack overflow report is formatted without unwinding.
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_STACK_OVERFLOW;
    record.ExceptionAddress = reinterpret_cast<PVOID>(&WriteThrough);
    CONTEXT context{};
    char text[8192];
    const auto size = reporter.Format(text, sizeof(text), record, context, true);
    const std::string report(text, size);
    Check(report.find("FATAL") != std::string::npos && report.find("stack overflow") != std::string::npos, "fatal stack overflow");
    Check(report.find("(not unwound)") != std::string::npos, "stack overflow not unwound");
    Check(report.find("module steps") == std::string::npos, "cleared step omitted");

    fs::remove_all(root, error);
    std::cout << (failures ? "crash reporter tests FAILED\n" : "all crash reporter checks passed\n");
    return failures ? 1 : 0;
}
