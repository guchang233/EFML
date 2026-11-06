#pragma once
// Development module switching ("efml stage").
//
// `efml stage <package>` copies a package into modules\packages\, rewrites its module.json id to
// <id>.dev<N> (N grows per base id and EFML_HOME) and records the switch in modules\dev-pending.json.
// It never touches index.json, so a running game and its loaded DLLs are left alone.
// The loader applies the pending switches at the next game start, before the Host reads the index:
// the new dev id is enabled, the base id and older dev ids of the same base are disabled (only the
// previous dev build is kept for rollback, older ones are removed and their folders retired).
// The new index is validated as a whole and written with one atomic rename; on any error the old
// index stays exactly as it was and the reason goes to logs\loader.log.
//
// Files (all under <EFML_HOME>\modules\):
//   dev-pending.json   switches waiting for the next start: {"schema":1,"switches":[...]}
//   dev-pending.applying   claimed by a starting loader; re-applied if that start crashed
//   dev-pending.rejected   last pending file that could not be applied (kept for inspection)
//   dev-builds.json    last build number per base id: {"author.mod": 3}
#include <Windows.h>

#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace EFML::DevStage {

namespace fs = std::filesystem;
using J = nlohmann::json;

inline fs::path ModulesDir(const fs::path& root) { return root / L"modules"; }
inline fs::path IndexFile(const fs::path& root) { return ModulesDir(root) / L"index.json"; }
inline fs::path PendingFile(const fs::path& root) { return ModulesDir(root) / L"dev-pending.json"; }
inline fs::path ClaimedFile(const fs::path& root) { return ModulesDir(root) / L"dev-pending.applying"; }
inline fs::path RejectedFile(const fs::path& root) { return ModulesDir(root) / L"dev-pending.rejected"; }
inline fs::path BuildsFile(const fs::path& root) { return ModulesDir(root) / L"dev-builds.json"; }

inline std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

inline std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// Same rule as the Host: [A-Za-z0-9][A-Za-z0-9_.-]{0,95}, reserved prefixes rejected.
inline bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 96 || !std::isalnum(static_cast<unsigned char>(id[0]))) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') return false;
    }
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !lower.starts_with("efml.") && !lower.starts_with("betterendfield.") &&
        !lower.starts_with("betterendfieldnext.") && lower != "voice.character";
}

// "author.mod.dev12" -> 12 for base "author.mod"; anything else -> nullopt.
inline std::optional<unsigned> DevBuild(const std::string& id, const std::string& base) {
    const std::string prefix = base + ".dev";
    if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    const auto digits = id.substr(prefix.size());
    if (digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
    return static_cast<unsigned>(std::stoul(digits));
}

// Strips a trailing ".dev<digits>", so staging an already staged package keeps the original base.
inline std::string BaseId(const std::string& id) {
    const auto dot = id.rfind(".dev");
    if (dot == std::string::npos || dot == 0) return id;
    const auto base = id.substr(0, dot);
    return DevBuild(id, base) ? base : id;
}

inline bool SameBase(const std::string& id, const std::string& base) { return id == base || DevBuild(id, base).has_value(); }

inline std::optional<J> ReadJson(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    J value = J::parse(in, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return value;
}

// Write to a temp file, then replace the target with one rename.
inline bool WriteJsonAtomic(const fs::path& file, const J& value) {
    std::error_code ignored;
    fs::create_directories(file.parent_path(), ignored);
    const auto temp = file.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump(2);
        out.flush();
        if (!out) return false;
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// The checks the Host applies before it accepts an index (ModuleHost::ApplyIndex).
inline bool ValidIndex(const J& index, std::string& why) {
    if (!index.is_object() || index.value("schema", 0) != 1) { why = "index is not schema 1"; return false; }
    if (!index.contains("modules") || !index["modules"].is_array()) { why = "index has no module list"; return false; }
    std::set<std::string> present;
    for (const auto& record : index["modules"]) {
        if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) { why = "module record without id"; return false; }
        const auto id = record["id"].get<std::string>();
        if (!ValidId(id)) { why = "invalid module id " + id; return false; }
        if (!present.insert(id).second) { why = "duplicate module id " + id; return false; }
        if (!record.contains("directory") || !record["directory"].is_string()) { why = id + " has no directory"; return false; }
        if (record.contains("configuration") && !record["configuration"].is_object()) { why = id + " configuration is not an object"; return false; }
    }
    return true;
}

// ---- Staging (efml stage) ----------------------------------------------------------------------

inline J ReadPackageManifest(const fs::path& package) {
    const auto manifest = ReadJson(package / L"module.json");
    if (!manifest || !manifest->is_object()) throw std::runtime_error("module.json missing or not valid JSON in " + ToUtf8(package.wstring()));
    if (manifest->value("format", 0) != 1 || manifest->value("abi", 0) != 1) throw std::runtime_error("module.json must have format 1 and abi 1");
    if (!manifest->contains("libraries") && !manifest->contains("ui")) throw std::runtime_error("module.json declares neither libraries nor ui");
    const auto id = manifest->value("id", "");
    if (!ValidId(id)) throw std::runtime_error("module.json has no valid id");
    return *manifest;
}

// Highest build number this home has seen for the base: counter file, index and pending switches.
inline unsigned LastBuild(const fs::path& root, const std::string& base) {
    unsigned last = 0;
    if (const auto builds = ReadJson(BuildsFile(root)); builds && builds->is_object() && builds->contains(base) &&
        (*builds)[base].is_number_unsigned()) {
        last = (*builds)[base].get<unsigned>();
    }
    auto scan = [&](const std::optional<J>& file, const char* list) {
        if (!file || !file->is_object() || !file->contains(list) || !(*file)[list].is_array()) return;
        for (const auto& record : (*file)[list]) {
            if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) continue;
            if (const auto n = DevBuild(record["id"].get<std::string>(), base)) last = (std::max)(last, *n);
        }
    };
    scan(ReadJson(IndexFile(root)), "modules");
    scan(ReadJson(PendingFile(root)), "switches");
    scan(ReadJson(ClaimedFile(root)), "switches");
    return last;
}

struct StageResult {
    fs::path root;
    std::string base;
    std::string id;
    unsigned build = 0;
    fs::path directory;
};

// Copies the package into one home as <base>.dev<build> and queues the switch. Does not touch index.json.
inline StageResult StageInto(const fs::path& root, const fs::path& package, unsigned build) {
    const J manifest = ReadPackageManifest(package);
    StageResult result;
    result.root = root;
    result.base = BaseId(manifest.value("id", ""));
    result.build = build;
    result.id = result.base + ".dev" + std::to_string(build);
    if (!ValidId(result.id)) throw std::runtime_error("id too long for a dev suffix: " + result.id);

    const auto packages = ModulesDir(root) / L"packages";
    fs::create_directories(packages);
    const auto stamp = GetTickCount64() ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32);
    wchar_t suffix[32]{};
    swprintf_s(suffix, L"-%08llx", stamp & 0xffffffffull);
    result.directory = packages / (FromUtf8(result.id) + suffix);
    for (int n = 2; fs::exists(result.directory); ++n) result.directory = packages / (FromUtf8(result.id) + suffix + L"-" + std::to_wstring(n));

    try {
        fs::copy(package, result.directory, fs::copy_options::recursive);
        J copied = manifest;
        copied["id"] = result.id;
        if (!WriteJsonAtomic(result.directory / L"module.json", copied)) throw std::runtime_error("could not rewrite module.json");

        J pending = ReadJson(PendingFile(root)).value_or(J::object());
        if (!pending.is_object() || pending.value("schema", 0) != 1 || !pending["switches"].is_array()) {
            pending = {{"schema", 1}, {"switches", J::array()}};
        }
        auto& switches = pending["switches"];
        // Only the newest build of a base waits; an older pending one is superseded.
        for (auto it = switches.begin(); it != switches.end();) {
            if (it->is_object() && it->value("base", "") == result.base) {
                if (const auto old = it->value("directory", ""); !old.empty()) {
                    std::error_code ignored;
                    fs::remove_all(FromUtf8(old), ignored);   // never loaded, safe to delete
                }
                it = switches.erase(it);
            } else {
                ++it;
            }
        }
        switches.push_back({{"base", result.base}, {"id", result.id}, {"directory", ToUtf8(result.directory.wstring())},
            {"generation", result.id + ToUtf8(suffix)}, {"default_configuration", manifest.value("default_configuration", J::object())}});
        J builds = ReadJson(BuildsFile(root)).value_or(J::object());
        if (!builds.is_object()) builds = J::object();
        builds[result.base] = build;
        if (!WriteJsonAtomic(BuildsFile(root), builds)) throw std::runtime_error("could not write " + ToUtf8(BuildsFile(root).wstring()));
        if (!WriteJsonAtomic(PendingFile(root), pending)) throw std::runtime_error("could not write " + ToUtf8(PendingFile(root).wstring()));
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(result.directory, ignored);
        throw;
    }
    return result;
}

// Stages one package into several homes with one shared build number (host and guest stay in step).
inline std::vector<StageResult> StageMany(const std::vector<fs::path>& roots, const fs::path& package) {
    const auto base = BaseId(ReadPackageManifest(package).value("id", ""));
    unsigned build = 0;
    for (const auto& root : roots) build = (std::max)(build, LastBuild(root, base));
    ++build;
    std::vector<StageResult> results;
    for (const auto& root : roots) results.push_back(StageInto(root, package, build));
    return results;
}

// ---- Applying at game start --------------------------------------------------------------------

// Pure: returns the index with the switches applied, or throws if a switch is unusable.
inline J ApplySwitches(J index, const J& pending) {
    if (!pending.is_object() || pending.value("schema", 0) != 1 || !pending.contains("switches") || !pending["switches"].is_array()) {
        throw std::runtime_error("pending file is not schema 1");
    }
    auto& modules = index["modules"];
    for (const auto& change : pending["switches"]) {
        if (!change.is_object()) throw std::runtime_error("pending switch is not an object");
        const auto base = change.value("base", "");
        const auto id = change.value("id", "");
        const auto directory = change.value("directory", "");
        const auto build = DevBuild(id, base);
        if (!ValidId(base) || !build) throw std::runtime_error("pending switch has an invalid id: " + id);
        const auto manifest = ReadJson(fs::path(FromUtf8(directory)) / L"module.json");
        if (directory.empty() || !manifest || manifest->value("id", "") != id) {
            throw std::runtime_error("staged package missing or mismatched for " + id + ": " + directory);
        }

        // Configuration follows the module: newest enabled record of the same base, else the base id.
        J configuration = change.value("default_configuration", J::object());
        long long best = -1;
        for (const auto& record : modules) {
            const auto other = record.value("id", "");
            if (!SameBase(other, base) || !record.contains("configuration")) continue;
            const long long score = (record.value("enabled", false) ? (1ll << 40) : 0) + DevBuild(other, base).value_or(0);
            if (score > best) {
                configuration = record["configuration"];
                best = score;
            }
        }

        // Keep the newest older dev build (disabled) for rollback; remove the rest.
        unsigned previous = 0;
        for (const auto& record : modules) {
            const auto n = DevBuild(record.value("id", ""), base);
            if (n && *n < *build) previous = (std::max)(previous, *n);
        }
        J kept = J::array();
        std::optional<size_t> slot;
        for (auto& record : modules) {
            const auto other = record.value("id", "");
            if (!SameBase(other, base)) { kept.push_back(record); continue; }
            if (!slot) slot = kept.size();
            const auto n = DevBuild(other, base);
            if (other == id || (n && *n != previous)) {
                if (other != id) index["retired_directories"].push_back(record.value("directory", ""));
                continue;
            }
            record["enabled"] = false;
            kept.push_back(record);
        }
        const J entry = {{"id", id}, {"directory", directory}, {"generation", change.value("generation", id)},
            {"enabled", true}, {"configuration", configuration}};
        kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(slot.value_or(kept.size())), entry);
        modules = kept;
    }
    return index;
}

struct ApplyOutcome {
    bool attempted = false;   // a pending file existed
    bool applied = false;     // at least one pending file reached index.json
    bool failed = false;      // a pending file was rejected or postponed (index left unchanged by it)
    std::vector<std::string> log;
};

// Applies dev-pending.json to index.json. Call before the Host loads; never throws.
inline ApplyOutcome ApplyPending(const fs::path& root) {
    ApplyOutcome outcome;
    try {
        // A claimed file left by a start that crashed goes first, then a newer pending one.
        for (int round = 0; round < 2; ++round) {
            const auto claimed = ClaimedFile(root);
            if (!fs::exists(claimed)) {
                if (!fs::exists(PendingFile(root))) break;
                if (!MoveFileExW(PendingFile(root).c_str(), claimed.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    outcome.attempted = true;
                    outcome.failed = true;
                    outcome.log.push_back("dev switch: could not claim dev-pending.json (error " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            outcome.attempted = true;
            const auto pending = ReadJson(claimed);
            auto reject = [&](const std::string& why) {
                MoveFileExW(claimed.c_str(), RejectedFile(root).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                outcome.log.push_back("dev switch rejected, module list unchanged: " + why + " (see dev-pending.rejected)");
            };
            if (!pending) { reject("dev-pending is not valid JSON"); outcome.failed = true; continue; }
            const auto index = ReadJson(IndexFile(root));
            std::string why;
            if (!index || !ValidIndex(*index, why)) {
                // Keep the claim: the switch is retried on the next start once the index is repaired.
                outcome.log.push_back("dev switch postponed, module list unchanged: index.json is " +
                    std::string(index ? "invalid (" + why + ")" : "missing or not JSON"));
                outcome.failed = true;
                break;
            }
            J next;
            try {
                next = ApplySwitches(*index, *pending);
            } catch (const std::exception& error) {
                reject(error.what());
                outcome.failed = true;
                continue;
            }
            if (!ValidIndex(next, why)) { reject("result would be invalid: " + why); outcome.failed = true; continue; }
            if (!WriteJsonAtomic(IndexFile(root), next)) {
                outcome.log.push_back("dev switch postponed, module list unchanged: could not write index.json");
                outcome.failed = true;
                break;
            }
            DeleteFileW(claimed.c_str());
            std::string ids;
            for (const auto& change : (*pending)["switches"]) ids += (ids.empty() ? "" : ", ") + change.value("id", "");
            outcome.log.push_back("dev switch applied: " + ids);
            outcome.applied = true;
        }
    } catch (const std::exception& error) {
        outcome.attempted = true;
        outcome.failed = true;
        outcome.log.push_back(std::string("dev switch failed, module list unchanged: ") + error.what());
    }
    return outcome;
}

} // namespace EFML::DevStage
