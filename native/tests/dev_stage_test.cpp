// Tests for `efml stage` and the loader's pending-switch apply (native/loader/dev_stage.h).
// Runs in a temporary EFML_HOME; needs no game and no module DLL.
#include "../loader/dev_stage.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using J = nlohmann::json;
namespace DS = EFML::DevStage;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const std::string& name) {
    ++checks;
    std::cout << (condition ? "  ok   " : "  FAIL ") << name << "\n";
    if (!condition) ++failures;
}

std::string Bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

J Index(const fs::path& root) { return DS::ReadJson(DS::IndexFile(root)).value_or(J()); }

const J* Record(const J& index, const std::string& id) {
    for (const auto& record : index["modules"]) {
        if (record.value("id", "") == id) return &record;
    }
    return nullptr;
}

bool Enabled(const J& index, const std::string& id) {
    const auto* record = Record(index, id);
    return record && record->value("enabled", false);
}

std::vector<std::string> EnabledOfBase(const J& index, const std::string& base) {
    std::vector<std::string> ids;
    for (const auto& record : index["modules"]) {
        if (DS::SameBase(record.value("id", ""), base) && record.value("enabled", false)) ids.push_back(record.value("id", ""));
    }
    return ids;
}

void FreshHome(const fs::path& root) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(DS::ModulesDir(root));
    // An installed release of the same module plus an unrelated one.
    const J index = {{"schema", 1}, {"port", 45678}, {"token", std::string(64, 'a')}, {"modules", J::array({
        {{"id", "test.mod"}, {"directory", "C:/release/test.mod"}, {"generation", "g0"}, {"enabled", true},
            {"configuration", {{"from", "release"}}}},
        {{"id", "other.mod"}, {"directory", "C:/release/other.mod"}, {"generation", "g1"}, {"enabled", true}}})}};
    DS::WriteJsonAtomic(DS::IndexFile(root), index);
}

} // namespace

int wmain(int, wchar_t**) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const fs::path scratch = fs::path(temp) / (L"efml-stage-test-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path home = scratch / L"host";
    const fs::path guest = scratch / L"guest";
    const fs::path package = scratch / L"package";
    WriteText(package / L"module.json",
        R"({"format":1,"abi":1,"id":"test.mod","version":"1.0.0","libraries":{"windows-x64":"native/m.dll"},"default_configuration":{"from":"default"}})");
    WriteText(package / L"native" / L"m.dll", "not a real dll");

    std::cout << "id helpers\n";
    Check(DS::BaseId("test.mod.dev12") == "test.mod" && DS::BaseId("test.mod") == "test.mod" &&
        DS::BaseId("test.devices") == "test.devices", "BaseId strips only .dev<digits>");

    std::cout << "staging\n";
    FreshHome(home);
    const auto index_before = Bytes(DS::IndexFile(home));
    const auto first = DS::StageMany({home}, package);
    const auto second = DS::StageMany({home}, package);
    Check(first[0].id == "test.mod.dev1" && second[0].id == "test.mod.dev2", "build number grows across stages (dev1, dev2)");
    Check(Bytes(DS::IndexFile(home)) == index_before, "stage does not touch index.json");
    Check(DS::ReadJson(second[0].directory / L"module.json").value_or(J()).value("id", "") == "test.mod.dev2" &&
        fs::exists(second[0].directory / L"native" / L"m.dll"), "package copied with rewritten id");
    const auto pending = DS::ReadJson(DS::PendingFile(home)).value_or(J());
    Check(pending["switches"].size() == 1 && pending["switches"][0].value("id", "") == "test.mod.dev2",
        "only the newest staged build of a base is pending");
    Check(!fs::exists(first[0].directory), "superseded, never-loaded staged copy is deleted");
    // Staging an already staged package keeps the original base.
    const auto restaged = DS::StageMany({home}, second[0].directory);
    Check(restaged[0].id == "test.mod.dev3", "staging a .devN package continues from the base id");

    std::cout << "simulated start\n";
    auto outcome = DS::ApplyPending(home);
    J index = Index(home);
    Check(outcome.attempted && outcome.applied && !outcome.failed, "pending switch applied");
    Check(EnabledOfBase(index, "test.mod") == std::vector<std::string>{"test.mod.dev3"}, "only the newest dev id is enabled");
    Check(Record(index, "test.mod") && !Enabled(index, "test.mod"), "release id kept but disabled");
    Check(Enabled(index, "other.mod"), "unrelated module untouched");
    Check(Record(index, "test.mod.dev3")->at("configuration").value("from", "") == "release", "configuration carried over");
    Check(index["modules"][0].value("id", "") == "test.mod.dev3", "dev id takes the module's place in load order");
    Check(!fs::exists(DS::PendingFile(home)) && !fs::exists(DS::ClaimedFile(home)), "pending file consumed");
    Check(!DS::ApplyPending(home).attempted, "second start has nothing to apply");

    std::cout << "counter after apply, rollback slot\n";
    const auto fourth = DS::StageMany({home}, package);
    Check(fourth[0].id == "test.mod.dev4", "build number continues after an apply");
    fs::remove(DS::BuildsFile(home));
    const auto fifth = DS::StageMany({home}, package);
    Check(fifth[0].id == "test.mod.dev5", "build number recovered from index and pending without the counter file");
    DS::ApplyPending(home);
    index = Index(home);
    Check(EnabledOfBase(index, "test.mod") == std::vector<std::string>{"test.mod.dev5"}, "dev5 is the only enabled build");
    Check(Record(index, "test.mod.dev3") && !Enabled(index, "test.mod.dev3"), "previous dev build kept disabled for rollback");
    const auto dev3_directory = Record(index, "test.mod.dev3")->value("directory", "");
    DS::StageMany({home}, package);   // dev6
    DS::ApplyPending(home);
    index = Index(home);
    bool retired = false;
    for (const auto& d : index["retired_directories"]) retired = retired || d.get<std::string>() == dev3_directory;
    Check(!Record(index, "test.mod.dev3") && Record(index, "test.mod.dev5") && !Enabled(index, "test.mod.dev5") &&
        index["modules"].size() == 4 && retired, "builds older than the previous one removed and their folders retired");

    std::cout << "atomic apply and rollback\n";
    DS::StageMany({home}, package);   // dev7 pending
    WriteText(DS::IndexFile(home), "{ this is not json");
    const auto broken = Bytes(DS::IndexFile(home));
    outcome = DS::ApplyPending(home);
    Check(outcome.failed && !outcome.applied && Bytes(DS::IndexFile(home)) == broken, "unreadable index left byte-for-byte unchanged");
    Check(fs::exists(DS::ClaimedFile(home)), "switch kept for the next start");

    FreshHome(home);
    const auto good = Bytes(DS::IndexFile(home));
    // Index that the Host would reject: duplicate ids.
    DS::StageMany({home}, package);
    J duplicate = Index(home);
    duplicate["modules"].push_back(duplicate["modules"][1]);
    DS::WriteJsonAtomic(DS::IndexFile(home), duplicate);
    const auto dup_bytes = Bytes(DS::IndexFile(home));
    outcome = DS::ApplyPending(home);
    Check(outcome.failed && Bytes(DS::IndexFile(home)) == dup_bytes, "invalid index (duplicate id) left unchanged");
    fs::remove(DS::ClaimedFile(home));

    DS::WriteJsonAtomic(DS::IndexFile(home), J::parse(good));
    const auto good_bytes = Bytes(DS::IndexFile(home));
    // Two switches; the second points at a staged folder that has gone. Neither may land.
    const auto a = DS::StageMany({home}, package);
    WriteText(scratch / L"other" / L"module.json",
        R"({"format":1,"abi":1,"id":"other.mod","libraries":{"windows-x64":"native/o.dll"}})");
    const auto b = DS::StageMany({home}, scratch / L"other");
    fs::remove_all(b[0].directory);
    outcome = DS::ApplyPending(home);
    Check(outcome.failed && !outcome.applied, "switch list with a missing package rejected");
    Check(Bytes(DS::IndexFile(home)) == good_bytes, "rejected switch list changes nothing (no partial apply)");
    Check(fs::exists(DS::RejectedFile(home)) && !fs::exists(DS::ClaimedFile(home)), "rejected list moved aside");
    bool logged = false;
    for (const auto& line : outcome.log) logged = logged || line.find("module list unchanged") != std::string::npos;
    Check(logged, "rejection explained in the log");

    WriteText(DS::PendingFile(home), "garbage");
    outcome = DS::ApplyPending(home);
    Check(outcome.failed && Bytes(DS::IndexFile(home)) == good_bytes, "corrupt pending file rejected, index unchanged");

    std::cout << "crash between index write and pending delete\n";
    DS::StageMany({home}, package);
    fs::copy_file(DS::PendingFile(home), scratch / L"saved-pending");
    DS::ApplyPending(home);
    const auto applied_once = Index(home);
    fs::copy_file(scratch / L"saved-pending", DS::ClaimedFile(home));   // as if the delete never happened
    DS::ApplyPending(home);
    Check(Index(home)["modules"] == applied_once["modules"], "re-applying a claimed switch is idempotent");

    std::cout << "several homes\n";
    FreshHome(home);
    FreshHome(guest);
    DS::StageMany({guest}, package);
    DS::StageMany({guest}, package);   // guest is ahead: dev2
    const auto both = DS::StageMany({home, guest}, package);
    Check(both.size() == 2 && both[0].id == "test.mod.dev3" && both[1].id == "test.mod.dev3", "one build number shared by all homes");
    DS::ApplyPending(home);
    DS::ApplyPending(guest);
    Check(EnabledOfBase(Index(home), "test.mod") == std::vector<std::string>{"test.mod.dev3"} &&
        EnabledOfBase(Index(guest), "test.mod") == std::vector<std::string>{"test.mod.dev3"}, "each home switches to the shared build");

    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    std::cout << checks - failures << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
