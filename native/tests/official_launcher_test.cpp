// Launcher bridge tests without the game or the official launcher.
// Covers option parsing, argument quoting and launcher discovery rejections (non-PE, unsigned).
// Usage: efml-launcher-tests.exe
#include "../loader/official_launcher.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace EFML::Loader::Launch;

namespace {

int failures = 0;

void Check(bool condition, const std::string& name) {
    std::cout << (condition ? "  ok   " : "  FAIL ") << name << "\n";
    if (!condition) ++failures;
}

bool Contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

std::wstring SelfPath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

fs::path MakeRoot() {
    const auto root = fs::temp_directory_path() /
        ("efml-launcher-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    return root;
}

void TestQuoting() {
    std::cout << "quoting\n";
    Check(QuoteArgument(L"C:\\Games\\Endfield.exe") == L"\"C:\\Games\\Endfield.exe\"", "plain path");
    Check(QuoteArgument(L"C:\\a b\\Endfield.exe") == L"\"C:\\a b\\Endfield.exe\"", "path with space");
    Check(QuoteArgument(L"a\"b") == L"\"a\\\"b\"", "embedded quote");
    Check(QuoteArgument(L"a\\") == L"\"a\\\\\"", "trailing backslash");
}

void TestOptions(const fs::path& root) {
    std::cout << "options\n";
    const auto game = root / L"Endfield.exe";
    { std::ofstream(game, std::ios::binary) << "placeholder"; }

    const std::wstring game_text = game.wstring();
    const std::wstring parent_text = (root / L"Games.exe").wstring();
    const std::wstring home_text = (root / L"home").wstring();

    wchar_t a0[] = L"EFML.Launcher.exe";
    wchar_t a1[] = L"--game";
    wchar_t a2[] = L"";               // replaced below
    wchar_t a3[] = L"--parent";
    wchar_t a4[] = L"";               // replaced below
    wchar_t a5[] = L"--home";
    wchar_t a6[] = L"";               // replaced below
    wchar_t a7[] = L"--hide-console";
    wchar_t a8[] = L"--";
    wchar_t a9[] = L"-logFile";
    wchar_t a10[] = L"player.log";
    std::vector<std::wstring> storage = {game_text, parent_text, home_text};
    wchar_t* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    // Point the placeholders at the real strings; ParseOptions only reads them.
    argv[2] = storage[0].data();
    argv[4] = storage[1].data();
    argv[6] = storage[2].data();

    Options options;
    std::wstring error;
    Check(ParseOptions(11, argv, options, error), "valid command line parses");
    Check(options.game == fs::absolute(game), "game path resolved");
    Check(options.parent == fs::path(storage[1]), "parent captured");
    Check(options.home == fs::path(storage[2]), "home captured");
    Check(options.hide_console, "hide-console captured");
    Check(options.arguments.size() == 2 && options.arguments[0] == L"-logFile" &&
              options.arguments[1] == L"player.log", "game arguments kept after --");

    {
        wchar_t b0[] = L"EFML.Launcher.exe";
        wchar_t b1[] = L"--game";
        wchar_t b2[] = L"";                // replaced below
        wchar_t b3[] = L"--no-parent";
        wchar_t* no_parent_argv[] = {b0, b1, b2, b3};
        std::wstring no_parent_game = game_text;
        no_parent_argv[2] = no_parent_game.data();
        Options parsed;
        std::wstring parse_error;
        Check(ParseOptions(4, no_parent_argv, parsed, parse_error), "--no-parent parses");
        Check(parsed.no_parent, "--no-parent captured");
    }
    {
        wchar_t c0[] = L"EFML.Launcher.exe";
        wchar_t c1[] = L"--unknown";
        wchar_t* bad_argv[] = {c0, c1};
        Options parsed;
        std::wstring parse_error;
        Check(!ParseOptions(2, bad_argv, parsed, parse_error), "unknown option rejected");
        Check(!parse_error.empty(), "unknown option reports an error");
    }
    {
        const auto other = root / L"Other.exe";
        { std::ofstream(other, std::ios::binary) << "placeholder"; }
        std::wstring other_text = other.wstring();
        wchar_t d0[] = L"EFML.Launcher.exe";
        wchar_t d1[] = L"--game";
        wchar_t* wrong_argv[] = {d0, d1, other_text.data()};
        Options parsed;
        std::wstring parse_error;
        Check(!ParseOptions(3, wrong_argv, parsed, parse_error), "non-Endfield.exe rejected");
    }
}

void TestDiscovery(const fs::path& root) {
    std::cout << "discovery\n";
    const auto launcher_root = root / L"launcher";
    const auto versioned = launcher_root / L"1.6.0";
    fs::create_directories(versioned);
    { std::ofstream(versioned / L"Games.exe", std::ios::binary) << "not a PE"; }

    VerifiedLauncher launcher;
    std::wstring error;
    Check(!FindOfficialLauncher(versioned / L"Games.exe", launcher, error), "non-PE launcher rejected");
    Check(Contains(error, L"PE"), "non-PE error names the PE check");

    // A real, unsigned PE: the client would reject it, so discovery must reject it too.
    std::error_code ignored;
    fs::copy_file(SelfPath(), launcher_root / L"Launcher.exe", fs::copy_options::overwrite_existing, ignored);
    fs::copy_file(SelfPath(), versioned / L"Games.exe", fs::copy_options::overwrite_existing, ignored);
    VerifiedLauncher resolved;
    std::wstring resolve_error;
    const bool accepted = FindOfficialLauncher(launcher_root / L"Launcher.exe", resolved, resolve_error);
    Check(!accepted, "unsigned launcher rejected");
    Check(Contains(resolve_error, L"1.6.0") && Contains(resolve_error, L"Games.exe"),
        "Launcher.exe resolves to the versioned Games.exe and reports it");
    Check(Contains(resolve_error, L"签名") || Contains(resolve_error, L"鹰角"),
        "rejection names the signature check");
}

} // namespace

int main() {
    const auto root = MakeRoot();
    TestQuoting();
    TestOptions(root);
    TestDiscovery(root);
    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::cout << (failures == 0 ? "all launcher bridge checks passed\n" : "launcher bridge checks failed\n");
    return failures == 0 ? 0 : 1;
}
