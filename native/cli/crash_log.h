#pragma once
// Reads <data root>\crash\crash.log (written by the host's CrashReporter) into JSON.
// Shared by `efml crash` and the crash reporter tests, so the format and the parser stay
// in step.
#include "../vendor/nlohmann/json.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace EFML::CrashLog {

using J = nlohmann::json;

inline std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

inline bool StartsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// "GameAssembly.dll+0x27C1F  Ns.Class::Method(...) +0x15  [module eflink.core]"
inline J ParseLocation(const std::string& text) {
    J out = J::object();
    std::string rest = text;
    const auto module_at = rest.find("  [module ");
    if (module_at != std::string::npos) {
        const auto end = rest.find(']', module_at);
        out["module"] = rest.substr(module_at + 10, end == std::string::npos ? std::string::npos : end - module_at - 10);
        rest = rest.substr(0, module_at);
    }
    const auto label_at = rest.find("  ");
    std::string address = label_at == std::string::npos ? rest : rest.substr(0, label_at);
    if (label_at != std::string::npos) {
        std::string label = Trim(rest.substr(label_at + 2));
        const auto offset_at = label.rfind(" +0x");
        if (offset_at != std::string::npos) {
            out["method_offset"] = label.substr(offset_at + 2);
            label = label.substr(0, offset_at);
        }
        out["method"] = label;
    }
    out["address"] = address;
    const auto plus = address.find('+');
    if (plus != std::string::npos) {
        out["image"] = address.substr(0, plus);
        out["offset"] = address.substr(plus + 1);
    }
    return out;
}

inline J ParseReport(const std::vector<std::string>& lines) {
    J report = {{"steps", J::array()}, {"modules_on_stack", J::array()}, {"stack", J::array()}, {"registers", J::object()}};
    // ==== 2026-11-06 16:06:55.968 pid 1 tid 2 Endfield.exe first-chance (...)
    {
        std::istringstream head(lines[0].substr(5));
        std::string date, time, word;
        head >> date >> time;
        report["time"] = date + " " + time;
        while (head >> word) {
            if (word == "pid") { head >> word; report["pid"] = std::strtoul(word.c_str(), nullptr, 10); }
            else if (word == "tid") { head >> word; report["tid"] = std::strtoul(word.c_str(), nullptr, 10); head >> word; report["process"] = word; break; }
        }
        report["fatal"] = lines[0].find("FATAL") != std::string::npos;
    }
    bool in_steps = false, in_stack = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto& raw = lines[i];
        const auto line = Trim(raw);
        if (line.empty()) continue;
        if (i == 1) {
            // "access violation 0xC0000005, read 0x138 at GameAssembly.dll+0x27C1F ..."
            const auto at = line.find(" at ");
            std::string head = at == std::string::npos ? line : line.substr(0, at);
            if (at != std::string::npos) report["location"] = ParseLocation(line.substr(at + 4));
            const auto comma = head.find(", ");
            if (comma != std::string::npos) {
                std::istringstream access(head.substr(comma + 2));
                std::string kind, target;
                access >> kind >> target;
                report["access"] = {{"kind", kind}, {"address", target}};
                head = head.substr(0, comma);
            }
            const auto code_at = head.rfind(" 0x");
            report["exception"] = code_at == std::string::npos ? head : head.substr(0, code_at);
            if (code_at != std::string::npos) report["code"] = head.substr(code_at + 1);
            continue;
        }
        if (line == "module steps:") { in_steps = true; in_stack = false; continue; }
        if (line == "stack:") { in_stack = true; in_steps = false; continue; }
        if (StartsWith(line, "modules on this stack: ")) {
            in_steps = false;
            std::istringstream ids(line.substr(23));
            std::string id;
            while (std::getline(ids, id, ',')) report["modules_on_stack"].push_back(Trim(id));
            continue;
        }
        if (in_stack) {
            if (StartsWith(line, "#")) {
                const auto space = line.find(' ');
                J frame = ParseLocation(space == std::string::npos ? std::string{} : line.substr(space + 1));
                frame["index"] = std::atoi(line.c_str() + 1);
                report["stack"].push_back(std::move(frame));
            }
            continue;
        }
        if (in_steps) {
            // "eflink.core: world_tick detail (1200 ms ago)"
            const auto colon = line.find(": ");
            const auto ago = line.rfind(" (");
            if (colon != std::string::npos) {
                J step = {{"module", line.substr(0, colon)}};
                step["step"] = line.substr(colon + 2, (ago == std::string::npos ? line.size() : ago) - colon - 2);
                if (ago != std::string::npos) step["ms_ago"] = std::strtoull(line.c_str() + ago + 2, nullptr, 10);
                report["steps"].push_back(std::move(step));
            }
            continue;
        }
        // Register lines: "rax 0 rbx 1 ..."
        std::istringstream registers(line);
        std::string name, value;
        while (registers >> name >> value) report["registers"][name] = value;
    }
    return report;
}

// Every report in the log, oldest first. "process start" lines become {"process_start": ...}.
inline J Parse(const std::string& text) {
    J reports = J::array();
    std::vector<std::string> current;
    auto flush = [&] {
        if (!current.empty()) reports.push_back(ParseReport(current));
        current.clear();
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (StartsWith(line, "==== ")) {
            flush();
            if (line.find(" process start ") != std::string::npos) {
                reports.push_back({{"process_start", Trim(line.substr(5))}});
                continue;
            }
        }
        if (StartsWith(line, "==== ") || !current.empty()) current.push_back(line);
    }
    flush();
    return reports;
}

} // namespace EFML::CrashLog
