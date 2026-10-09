#pragma once
#include "EFML/Module.h"
#include "paths.h"

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace EFML::Host {

class DynamicResolver;
class GameThread;
class HookBroker;
class Logger;
class ModuleHost;

// Lives for the whole game process. Starts logging, the hook broker and the module host, then
// waits for IL2CPP, hands modules the runtime helpers and finds the game thread.
class HostRuntime {
public:
    HostRuntime(HMODULE host_module, const wchar_t* bootstrap_data_root);
    ~HostRuntime();

    void Run();
    void RequestStop();
    BE_Result RetireHooks(const std::string& module_id);

private:
    void StartGameThread();

    static void BE_CALL LogCallback(void* context, const char* module_id, const char* message);
    static BE_Result BE_CALL ResolveMethodCallback(void* context, const BE_MethodDescriptorV1* descriptor,
        BE_ResolvedMethodV1* result);
    static BE_Result BE_CALL ResolveFieldCallback(void* context, const BE_FieldDescriptorV1* descriptor,
        BE_ResolvedFieldV1* result);
    static BE_Result BE_CALL CreateHookCallback(void* context, const char* module_id, void* target,
        void* detour, void** original);
    static BE_Result BE_CALL ReleaseModuleHooksCallback(void* context, const char* module_id);
    static int BE_CALL CopyModuleConfigurationCallback(void* context, const char* module_id,
        char* destination, size_t destination_size);
    static int BE_CALL CopyCatalogRootCallback(void* context, char* destination, size_t destination_size);
    static int BE_CALL CopyManagedStringCallback(void* context, const void* managed_string,
        char* destination, size_t destination_size);
    static BE_Result BE_CALL ResolveClassCallback(void* context, const char* assembly_name,
        const char* namespace_name, const char* class_name, BE_ResolvedClassV1* result);
    static void* BE_CALL ObjectNewCallback(void* context, const void* class_info);
    static void* BE_CALL StringNewCallback(void* context, const char* utf8);
    static void* BE_CALL RuntimeInvokeCallback(void* context, const void* method_info, void* instance,
        void** parameters, void** exception);
    static void* BE_CALL ObjectUnboxCallback(void* context, void* boxed_value);
    static void* BE_CALL FieldGetValueObjectCallback(void* context, const void* field_info, void* instance);
    static uint32_t BE_CALL GCHandleNewCallback(void* context, void* object, int pinned);
    static void BE_CALL GCHandleFreeCallback(void* context, uint32_t handle);

    HMODULE host_module_ = nullptr;
    std::wstring bootstrap_data_root_;
    Paths paths_;
    std::string data_root_utf8_;
    BE_HostApiV1 api_{};
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<DynamicResolver> resolver_;
    std::unique_ptr<HookBroker> hooks_;
    std::unique_ptr<GameThread> game_thread_;
    std::unique_ptr<ModuleHost> modules_;
    std::atomic_bool stop_requested_{false};
};

} // namespace EFML::Host
