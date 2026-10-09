// Derived from Better-Endfield's bootstrap.cpp (https://github.com/Dr-hydra/Better-Endfield), AGPL-3.0.
#include "bootstrap_config.h"
#include "host_runtime.h"
#include "paths.h"

#include <Windows.h>

#include <cstring>
#include <new>
#include <string>

namespace {

HMODULE g_host_module = nullptr;
EFML_BootstrapConfigV1 g_bootstrap{};
bool g_has_bootstrap = false;
// Process-lifetime object: never destroyed, so no teardown runs under the loader lock.
EFML::Host::HostRuntime* g_runtime = nullptr;

// Written before logging exists, so a host that never starts still leaves a trace.
void WriteAttachMarker(const wchar_t* message) {
    const auto directory = EFML::Host::DefaultDataRoot() / L"logs";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const HANDLE file = CreateFileW((directory / L"host-attach.marker").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[256]{};
    const int length = _snprintf_s(line, _countof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u %lu %ls\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), message);
    DWORD written = 0;
    if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

DWORD WINAPI HostThread(void*) {
    // One Host per process, even if both the proxy and the injector loaded a copy.
    const std::wstring name = L"Local\\EFML.Host." + std::to_wstring(GetCurrentProcessId());
    const HANDLE single = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        WriteAttachMarker(L"another EFML Host already runs in this process");
        return 0;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_host_module), &pinned)) {
        WriteAttachMarker(L"Host pin failed");
        return 0;
    }
    WriteAttachMarker(L"Host started");
    g_runtime = new (std::nothrow) EFML::Host::HostRuntime(g_host_module,
        g_has_bootstrap ? g_bootstrap.data_root : nullptr);
    if (g_runtime) g_runtime->Run();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    g_host_module = instance;
    if (reserved) {
        __try {
            const auto* candidate = static_cast<const EFML_BootstrapConfigV1*>(reserved);
            if (candidate->size == sizeof(EFML_BootstrapConfigV1) &&
                std::memcmp(candidate->magic, EFML_BOOTSTRAP_MAGIC, 9) == 0) {
                std::memcpy(&g_bootstrap, candidate, sizeof(g_bootstrap));
                g_bootstrap.data_root[EFML_BOOTSTRAP_PATH_CAPACITY - 1] = L'\0';
                g_has_bootstrap = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_has_bootstrap = false;
        }
    }
    const HANDLE thread = CreateThread(nullptr, 0, HostThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    return TRUE;
}
