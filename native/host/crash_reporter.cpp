#include "crash_reporter.h"

#include <DbgHelp.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace EFML::Host {
namespace {

constexpr int kSlots = 64;
constexpr int kLabels = 8192;
constexpr int kFrames = 48;
constexpr int kMaxReports = 200;
constexpr int kSeen = 512;
constexpr uint32_t kHeapCorruption = 0xC0000374u;      // STATUS_HEAP_CORRUPTION
constexpr uint32_t kStackBufferOverrun = 0xC0000409u; // STATUS_STACK_BUFFER_OVERRUN

struct Slot {
    std::atomic<uintptr_t> base{0}, end{0};
    char id[64]{};
    char step[64]{};
    char detail[128]{};
    std::atomic<uint64_t> at{0};
};
struct Label {
    uintptr_t entry = 0;
    char text[160]{};
};

Slot g_slots[kSlots];
std::atomic<int> g_slot_count{0};
std::mutex g_register_mutex;   // writers only; the handler reads published counts
Label g_labels[kLabels];
std::atomic<int> g_label_count{0};

HANDLE g_file = INVALID_HANDLE_VALUE;
PVOID g_vectored = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
bool g_filter_installed = false;
std::atomic<uintptr_t> g_seen[kSeen];
std::atomic<int> g_written{0};
std::atomic_flag g_busy = ATOMIC_FLAG_INIT;
char g_buffer[24576];
CONTEXT g_unwind_context;
uintptr_t g_frames[kFrames];
char g_process[64] = "process";
uintptr_t g_exe_base = 0;
wchar_t g_dump_root[MAX_PATH]{};
bool g_minidumps = false;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_write_dump = nullptr;

void CopyTruncated(char* destination, size_t size, const char* source) {
    if (!source) { destination[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < size && source[i]; ++i) destination[i] = source[i];
    destination[i] = '\0';
}

bool Recorded(uint32_t code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case kHeapCorruption:
    case kStackBufferOverrun:
        return true;
    default:
        return false;
    }
}

const char* CodeName(uint32_t code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case kHeapCorruption: return "heap corruption";
    case kStackBufferOverrun: return "stack buffer overrun";
    default: return "exception";
    }
}

// The image's own name from its export directory (no loader lock). Null when unreadable.
const char* ExportName(uintptr_t base) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return nullptr;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        if (!exports->Name) return nullptr;
        const char* name = reinterpret_cast<const char*>(base + exports->Name);
        return name[0] ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uintptr_t ImageEnd(uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

const Slot* ModuleAt(uintptr_t address) {
    const int count = g_slot_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        const auto base = g_slots[i].base.load(std::memory_order_relaxed);
        if (base && address >= base && address < g_slots[i].end.load(std::memory_order_relaxed)) return &g_slots[i];
    }
    return nullptr;
}

const Label* LabelOf(uintptr_t function) {
    const int count = g_label_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) if (g_labels[i].entry == function) return &g_labels[i];
    return nullptr;
}

// Unwinds from the faulting context. A corrupt stack ends the walk instead of faulting again.
int Unwind(const CONTEXT& start, uintptr_t* frames, int max) {
    int count = 0;
    g_unwind_context = start;
    CONTEXT& c = g_unwind_context;
    __try {
        while (count < max) {
            const DWORD64 pc = c.Rip;
            if (!pc) break;
            frames[count++] = static_cast<uintptr_t>(pc);
            DWORD64 image = 0;
            auto* function = RtlLookupFunctionEntry(pc, &image, nullptr);
            if (function) {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, function, &c, &handler_data, &establisher, nullptr);
            } else {
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
            if (c.Rip == pc && count > 1 && frames[count - 2] == pc) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

struct Writer {
    char* out;
    size_t capacity, size = 0;
    void operator()(const char* format, ...) {
        if (size + 1 >= capacity) return;
        va_list args;
        va_start(args, format);
        const int n = vsnprintf(out + size, capacity - size, format, args);
        va_end(args);
        if (n > 0) size = std::min(capacity - 1, size + static_cast<size_t>(n));
    }
};

// "GameAssembly.dll+0x27C1F", plus the method name when the address is inside a resolved
// entry and the module id when it is inside an EFML module.
void WriteLocation(Writer& w, uintptr_t address) {
    PVOID base_pointer = nullptr;
    RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &base_pointer);
    const auto base = reinterpret_cast<uintptr_t>(base_pointer);
    if (!base) {
        w("0x%llX", static_cast<unsigned long long>(address));
        return;
    }
    const char* name = base == g_exe_base ? g_process : ExportName(base);
    if (name) w("%s+0x%llX", name, static_cast<unsigned long long>(address - base));
    else w("0x%llX+0x%llX", static_cast<unsigned long long>(base), static_cast<unsigned long long>(address - base));
    DWORD64 image = 0;
    if (const auto* function = RtlLookupFunctionEntry(address, &image, nullptr)) {
        const uintptr_t start = static_cast<uintptr_t>(image) + function->BeginAddress;
        if (const auto* label = LabelOf(start)) w("  %s +0x%llX", label->text, static_cast<unsigned long long>(address - start));
    }
    if (const auto* slot = ModuleAt(address)) w("  [module %s]", slot->id);
}

} // namespace

CrashReporter& CrashReporter::Instance() {
    static CrashReporter instance;
    return instance;
}

uint64_t CrashReporter::Recorded() const { return static_cast<uint64_t>(g_written.load()); }

size_t CrashReporter::Format(char* out, size_t capacity, const EXCEPTION_RECORD& record, const CONTEXT& context,
    bool fatal) const {
    Writer w{out, capacity};
    SYSTEMTIME t;
    GetLocalTime(&t);
    w("==== %04d-%02d-%02d %02d:%02d:%02d.%03d pid %lu tid %lu %s %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
        t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), g_process,
        fatal ? "FATAL (unhandled)" : "first-chance (the game may still handle it)");
    const uint32_t code = record.ExceptionCode;
    w("%s 0x%08lX", CodeName(code), code);
    if (code == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const auto kind = record.ExceptionInformation[0];
        w(", %s 0x%llX", kind == 1 ? "write" : kind == 8 ? "execute" : "read",
            static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    w(" at ");
    WriteLocation(w, reinterpret_cast<uintptr_t>(record.ExceptionAddress));
    w("\r\n");
    const auto& c = context;
    w("  rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX\r\n", c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi);
    w("  rbp %llX rsp %llX r8 %llX r9 %llX r10 %llX r11 %llX\r\n", c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11);
    w("  r12 %llX r13 %llX r14 %llX r15 %llX rip %llX\r\n", c.R12, c.R13, c.R14, c.R15, c.Rip);

    const uint64_t now = GetTickCount64();
    const int slots = g_slot_count.load(std::memory_order_acquire);
    bool marks = false;
    for (int i = 0; i < slots; ++i) {
        const auto& slot = g_slots[i];
        if (!slot.step[0]) continue;
        if (!marks) { w("  module steps:\r\n"); marks = true; }
        const uint64_t at = slot.at.load(std::memory_order_relaxed);
        w("    %s: %s%s%s (%llu ms ago)\r\n", slot.id, slot.step, slot.detail[0] ? " " : "", slot.detail,
            static_cast<unsigned long long>(at && now >= at ? now - at : 0));
    }

    int frames = 0;
    if (code != EXCEPTION_STACK_OVERFLOW) frames = Unwind(context, g_frames, kFrames);
    bool on_stack[kSlots]{};
    for (int i = 0; i < frames; ++i)
        if (const auto* slot = ModuleAt(g_frames[i])) on_stack[slot - g_slots] = true;
    bool any = false;
    for (int i = 0; i < slots; ++i) {
        if (!on_stack[i]) continue;
        w(any ? ", %s" : "  modules on this stack: %s", g_slots[i].id);
        any = true;
    }
    if (any) w("\r\n");
    w("  stack:\r\n");
    for (int i = 0; i < frames; ++i) {
        w("    #%02d ", i);
        WriteLocation(w, g_frames[i]);
        w("\r\n");
    }
    if (!frames) w("    (not unwound)\r\n");
    return w.size;
}

namespace {

void WriteReport(EXCEPTION_POINTERS* e, bool fatal) {
    if (g_file == INVALID_HANDLE_VALUE || g_busy.test_and_set(std::memory_order_acquire)) return;
    const size_t size = CrashReporter::Instance().Format(g_buffer, sizeof(g_buffer), *e->ExceptionRecord,
        *e->ContextRecord, fatal);
    DWORD written = 0;
    WriteFile(g_file, g_buffer, static_cast<DWORD>(size), &written, nullptr);
    FlushFileBuffers(g_file);
    g_written.fetch_add(1);
    g_busy.clear(std::memory_order_release);
}

LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* e) {
    static thread_local bool inside = false;
    if (inside || !e || !e->ExceptionRecord || !Recorded(e->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    for (auto& seen : g_seen) {
        auto value = seen.load(std::memory_order_relaxed);
        if (value == address) return EXCEPTION_CONTINUE_SEARCH;
        if (!value && seen.compare_exchange_strong(value, address)) break;
        if (value == address) return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_written.load() >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
    WriteReport(e, false);
    inside = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

void WriteMinidump(EXCEPTION_POINTERS* e) {
    if (!g_minidumps || !g_write_dump || !g_dump_root[0]) return;
    wchar_t path[MAX_PATH];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(path, L"%s\\dump-%04d%02d%02d-%02d%02d%02d-%lu.dmp", g_dump_root, t.wYear, t.wMonth, t.wDay, t.wHour,
        t.wMinute, t.wSecond, GetCurrentProcessId());
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithUnloadedModules), &info, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* e) {
    if (e && e->ExceptionRecord) {
        WriteReport(e, true);
        WriteMinidump(e);
    }
    return g_previous_filter ? g_previous_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}

void PruneDumps(const std::filesystem::path& root) {
    std::error_code error;
    std::vector<std::filesystem::directory_entry> dumps;
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
        if (entry.path().extension() == L".dmp") dumps.push_back(entry);
    std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) > b.last_write_time(e2);
    });
    for (size_t i = 5; i < dumps.size(); ++i) std::filesystem::remove(dumps[i].path(), error);
}

} // namespace

void CrashReporter::Install(const std::filesystem::path& crash_root, const std::string& process_name) {
    if (g_vectored) return;
    std::error_code error;
    std::filesystem::create_directories(crash_root, error);
    root_ = crash_root;
    log_path_ = crash_root / L"crash.log";
    // Keep the log bounded: one previous generation.
    if (std::filesystem::exists(log_path_, error) && std::filesystem::file_size(log_path_, error) > 4u * 1024 * 1024)
        std::filesystem::rename(log_path_, crash_root / L"crash.old.log", error);
    g_file = CreateFileW(log_path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;
    CopyTruncated(g_process, sizeof(g_process), process_name.c_str());
    g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    wcsncpy_s(g_dump_root, crash_root.c_str(), _TRUNCATE);
    char variable[8]{};
    g_minidumps = std::filesystem::exists(crash_root / L"minidump.on", error) ||
        (GetEnvironmentVariableA("EFML_MINIDUMP", variable, sizeof(variable)) && variable[0] == '1');
    if (g_minidumps) {
        if (const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        PruneDumps(crash_root);
    }
    char line[256];
    SYSTEMTIME t;
    GetLocalTime(&t);
    const int n = snprintf(line, sizeof(line), "==== %04d-%02d-%02d %02d:%02d:%02d process start pid %lu %s%s\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), g_process,
        g_minidumps ? " (minidumps on)" : "");
    DWORD written = 0;
    if (n > 0) WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    g_vectored = AddVectoredExceptionHandler(1, &OnVectoredException);
    g_previous_filter = SetUnhandledExceptionFilter(&OnUnhandledException);
    g_filter_installed = true;
}

void CrashReporter::Uninstall() {
    if (g_vectored) {
        RemoveVectoredExceptionHandler(g_vectored);
        g_vectored = nullptr;
    }
    if (g_filter_installed) {
        SetUnhandledExceptionFilter(g_previous_filter);
        g_filter_installed = false;
    }
    // Wait for a report in progress on another thread.
    while (g_busy.test_and_set(std::memory_order_acquire)) Sleep(1);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_busy.clear(std::memory_order_release);
}

int CrashReporter::RegisterModule(const std::string& module_id, HMODULE module) {
    if (!module) return -1;
    std::lock_guard lock(g_register_mutex);
    const auto base = reinterpret_cast<uintptr_t>(module);
    const int count = g_slot_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) if (g_slots[i].base.load() == base) return i;
    if (count >= kSlots) return -1;
    auto& slot = g_slots[count];
    CopyTruncated(slot.id, sizeof(slot.id), module_id.c_str());
    slot.end.store(ImageEnd(base), std::memory_order_relaxed);
    slot.base.store(base, std::memory_order_relaxed);
    g_slot_count.store(count + 1, std::memory_order_release);
    return count;
}

void CrashReporter::Mark(int slot, const char* step, const char* detail) {
    if (slot < 0 || slot >= g_slot_count.load(std::memory_order_acquire)) return;
    auto& s = g_slots[slot];
    // Torn text under a concurrent fault is acceptable; this never blocks.
    CopyTruncated(s.detail, sizeof(s.detail), step ? detail : nullptr);
    CopyTruncated(s.step, sizeof(s.step), step);
    s.at.store(GetTickCount64(), std::memory_order_relaxed);
}

void CrashReporter::AddLabel(const void* entry, const std::string& label) {
    if (!entry || label.empty()) return;
    std::lock_guard lock(g_register_mutex);
    const int count = g_label_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) if (g_labels[i].entry == reinterpret_cast<uintptr_t>(entry)) return;
    if (count >= kLabels) return;
    g_labels[count].entry = reinterpret_cast<uintptr_t>(entry);
    CopyTruncated(g_labels[count].text, sizeof(g_labels[count].text), label.c_str());
    g_label_count.store(count + 1, std::memory_order_release);
}

} // namespace EFML::Host
