// Derived from Better-Endfield's input_proxy.cpp (https://github.com/Dr-hydra/Better-Endfield), AGPL-3.0.
// Built twice:
//   EFML_XINPUT_PROXY  -> xinput1_4.dll placed in the game folder; the game loads it at startup.
//   EFML_BOOTSTRAP     -> EFML.Bootstrap.dll, loaded into the game by EFML.Injector.exe.
// Both start a worker that loads the Host once IL2CPP exists.
#include "loader_config.h"

#include <Windows.h>

#include <string>

#if defined(EFML_XINPUT_PROXY)
// PE forwarders to the real XInput; no LoadLibrary from DllMain.
#pragma comment(linker, "/export:XInputGetState=C:\\Windows\\System32\\XInput1_4.XInputGetState,@2")
#pragma comment(linker, "/export:XInputSetState=C:\\Windows\\System32\\XInput1_4.XInputSetState,@3")
#pragma comment(linker, "/export:XInputGetCapabilities=C:\\Windows\\System32\\XInput1_4.XInputGetCapabilities,@4")
#pragma comment(linker, "/export:XInputEnable=C:\\Windows\\System32\\XInput1_4.XInputEnable,@5")
#pragma comment(linker, "/export:XInputGetBatteryInformation=C:\\Windows\\System32\\XInput1_4.XInputGetBatteryInformation,@7")
#pragma comment(linker, "/export:XInputGetKeystroke=C:\\Windows\\System32\\XInput1_4.XInputGetKeystroke,@8")
#pragma comment(linker, "/export:XInputGetAudioDeviceIds=C:\\Windows\\System32\\XInput1_4.XInputGetAudioDeviceIds,@10")
// Lets the manager and the injector recognise this file as EFML's proxy.
extern "C" __declspec(dllexport) const char EFML_ProxyMarker[] = "EFML-XInput-Proxy";
#define EFML_LOADER_NAME "xinput proxy"
#elif defined(EFML_BOOTSTRAP)
#define EFML_LOADER_NAME "bootstrap"
#else
#error Select EFML_XINPUT_PROXY or EFML_BOOTSTRAP.
#endif

namespace {

using DomainGetFn = void* (*)();
using ThreadAttachFn = void* (*)(void*);
using ThreadDetachFn = void (*)(void*);

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, nullptr);
    return output;
}

DWORD WINAPI Worker(void*) {
    const auto root = EFML::Loader::DataRoot();
    EFML::Loader::LoaderLog(root, EFML_LOADER_NAME " started");
    if (!EFML::Loader::ShouldLoadHost(root)) {
        EFML::Loader::LoaderLog(root, "Host disabled by loader.ini");
        return 0;
    }
    const auto host = EFML::Loader::HostPath(root);
    if (host.empty() || !std::filesystem::is_regular_file(host)) {
        EFML::Loader::LoaderLog(root, "Host not found: " + Narrow(host.wstring()) + " (install the runtime from the EFML manager)");
        return 0;
    }
#if defined(EFML_BOOTSTRAP)
    // The injector resumed the game already; the Host attaches to IL2CPP itself when ready.
    const HMODULE loaded = LoadLibraryExW(host.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    EFML::Loader::LoaderLog(root, loaded ? "Host loaded" : "Host load failed: " + std::to_string(GetLastError()));
#else
    // Loaded with the game's imports, long before Unity; wait for the IL2CPP domain.
    DomainGetFn domain_get = nullptr;
    ThreadAttachFn thread_attach = nullptr;
    ThreadDetachFn thread_detach = nullptr;
    void* domain = nullptr;
    for (int attempt = 0; attempt < 600 && !domain; ++attempt) {
        if (const HMODULE game_assembly = GetModuleHandleW(L"GameAssembly.dll"); game_assembly && !domain_get) {
            domain_get = reinterpret_cast<DomainGetFn>(GetProcAddress(game_assembly, "il2cpp_domain_get"));
            thread_attach = reinterpret_cast<ThreadAttachFn>(GetProcAddress(game_assembly, "il2cpp_thread_attach"));
            thread_detach = reinterpret_cast<ThreadDetachFn>(GetProcAddress(game_assembly, "il2cpp_thread_detach"));
        }
        if (domain_get && thread_attach && thread_detach) domain = domain_get();
        if (!domain) Sleep(500);
    }
    void* attached = domain ? thread_attach(domain) : nullptr;
    if (!attached) {
        EFML::Loader::LoaderLog(root, "IL2CPP domain not available; Host not loaded");
        return 0;
    }
    const HMODULE loaded = LoadLibraryExW(host.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    thread_detach(attached);
    EFML::Loader::LoaderLog(root, loaded ? "Host loaded" : "Host load failed: " + std::to_string(GetLastError()));
#endif
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
