#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace EFML::Host {

// Process-wide fault log, written to <data root>\crash\crash.log.
//
// A vectored handler records faulting exceptions as the game raises them: code, faulting
// address as module+offset, registers, the stack unwound from the faulting context, which
// EFML modules are on that stack and the step each module last marked. Exceptions are
// recorded at first chance, so the game may still handle some of them (each address is
// written once per process). The handler never handles or swallows an exception.
//
// The handler allocates nothing and takes no lock: it formats into a static buffer, reads
// module names from PE export directories and unwinds with RtlVirtualUnwind.
class CrashReporter {
public:
    static CrashReporter& Instance();

    // Idempotent. Also installs an unhandled-exception filter that marks fatal faults and,
    // when <crash root>\minidump.on exists or EFML_MINIDUMP=1, writes a minidump (the five
    // newest are kept).
    void Install(const std::filesystem::path& crash_root, const std::string& process_name);
    void Uninstall();

    // Modules are pinned for the process lifetime, so slots are never reused. Returns -1
    // when the table is full.
    int RegisterModule(const std::string& module_id, HMODULE module);
    // What the module is doing now (copied, truncated). step null clears it.
    void Mark(int slot, const char* step, const char* detail);
    // A resolved method entry: frames inside that function are printed with this label.
    void AddLabel(const void* entry, const std::string& label);

    const std::filesystem::path& LogPath() const { return log_path_; }
    uint64_t Recorded() const;

    // Formats one report into out (used by the handler and by tests).
    size_t Format(char* out, size_t capacity, const EXCEPTION_RECORD& record, const CONTEXT& context,
        bool fatal) const;

private:
    CrashReporter() = default;
    std::filesystem::path root_, log_path_;
};

} // namespace EFML::Host
