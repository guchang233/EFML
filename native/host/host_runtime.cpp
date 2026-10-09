// Derived from Better-Endfield's host_runtime.cpp (https://github.com/Dr-hydra/Better-Endfield), AGPL-3.0.
#include "host_runtime.h"

#include "dynamic_resolver.h"
#include "game_thread.h"
#include "hook_broker.h"
#include "logging.h"
#include "module_host.h"
#include "version.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace EFML::Host {
namespace {

constexpr auto kRuntimeWait = std::chrono::minutes(5);
constexpr auto kRuntimePoll = std::chrono::milliseconds(100);
// GameAssembly can become visible before Unity has registered the collector's threads;
// attaching earlier can end the process with "Collecting from unknown thread".
constexpr auto kRuntimeSettle = std::chrono::seconds(3);

std::string MethodLabel(const BE_MethodDescriptorV1& descriptor) {
    std::string label;
    if (descriptor.namespace_name && *descriptor.namespace_name) label = std::string(descriptor.namespace_name) + ".";
    label += std::string(descriptor.class_name ? descriptor.class_name : "?") + "::" +
        (descriptor.method_name ? descriptor.method_name : "?") + "(";
    label += descriptor.parameter_types ? std::string(descriptor.parameter_types)
                                        : std::to_string(descriptor.parameter_count) + " args";
    return label + ")";
}

int CopyText(const std::string& text, char* destination, size_t destination_size) {
    if (!destination || destination_size == 0) return 0;
    const size_t size = std::min(text.size(), destination_size - 1);
    std::memcpy(destination, text.data(), size);
    destination[size] = '\0';
    return static_cast<int>(size);
}

std::string ProcessName() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return ToUtf8(std::filesystem::path(path).filename().wstring());
}

} // namespace

// Optional named export, kept from Better-Endfield: turns a module's hooks into pass-throughs.
BE_EXPORT BE_Result BE_CALL EFML_RetireModuleHooksV1(void* context, const char* module_id) {
    if (!context || !module_id || !*module_id) return BE_Result_InvalidArgument;
    return static_cast<HostRuntime*>(context)->RetireHooks(module_id);
}

HostRuntime::HostRuntime(HMODULE host_module, const wchar_t* bootstrap_data_root)
    : host_module_(host_module), bootstrap_data_root_(bootstrap_data_root ? bootstrap_data_root : L"") {
    api_.abi_version = EFML_MODULE_ABI_V1;
    api_.context = this;
    api_.log = &LogCallback;
    api_.resolve_method = &ResolveMethodCallback;
    api_.resolve_field = &ResolveFieldCallback;
    api_.create_hook = &CreateHookCallback;
    api_.release_module_hooks = &ReleaseModuleHooksCallback;
    api_.copy_module_configuration = &CopyModuleConfigurationCallback;
    api_.copy_catalog_root = &CopyCatalogRootCallback;
    api_.copy_managed_string = &CopyManagedStringCallback;
    api_.resolve_class = &ResolveClassCallback;
    api_.object_new = &ObjectNewCallback;
    api_.string_new = &StringNewCallback;
    api_.runtime_invoke = &RuntimeInvokeCallback;
    api_.object_unbox = &ObjectUnboxCallback;
    api_.gchandle_new = &GCHandleNewCallback;
    api_.gchandle_free = &GCHandleFreeCallback;
    api_.field_get_value_object = &FieldGetValueObjectCallback;
}

HostRuntime::~HostRuntime() {
    if (modules_) modules_->Stop();
    if (hooks_) hooks_->Shutdown();
    if (resolver_) resolver_->DetachCurrentThread();
}

void HostRuntime::Run() {
    paths_ = DiscoverPaths(bootstrap_data_root_.c_str());
    data_root_utf8_ = ToUtf8(paths_.data_root.wstring());
    logger_ = std::make_unique<Logger>();
    logger_->Initialize(paths_.log_root);
    logger_->Write("host", "EFML " EFML_VERSION " starting in " + ProcessName() + "; data " + data_root_utf8_);
    for (const wchar_t* other : {L"BetterEndfieldNext.Host.dll", L"BetterEndfield.Host.dll"}) {
        if (GetModuleHandleW(other)) {
            logger_->Write("host", "Warning: " + ToUtf8(other) +
                " is also loaded. Both hosts hook the game independently; use one of them.");
        }
    }

    hooks_ = std::make_unique<HookBroker>(*logger_);
    const bool hooks_ready = hooks_->Initialize();
    if (!hooks_ready) logger_->Write("host", "Hook broker unavailable; modules cannot hook.");
    game_thread_ = std::make_unique<GameThread>(*logger_);
    modules_ = std::make_unique<ModuleHost>();
    ModuleHost::Options options;
    options.index = paths_.index_file;
    options.data_root = data_root_utf8_;
    options.log = [this](const std::string& source, const std::string& message) { logger_->Write(source, message); };
    options.hooks = hooks_ready ? hooks_->ChainApi() : nullptr;
    options.game_thread = game_thread_.get();
    if (!modules_->Start(std::move(options))) logger_->Write("host", "Module host did not start.");
    game_thread_->on_ready = [this] {
        modules_->PublishEvent("{\"type\":\"game_thread_ready\",\"source\":\"" + game_thread_->SourceName() + "\"}");
    };

    const auto deadline = std::chrono::steady_clock::now() + kRuntimeWait;
    while (!GetModuleHandleW(L"GameAssembly.dll")) {
        if (std::chrono::steady_clock::now() >= deadline || stop_requested_.load()) {
            logger_->Write("host", "GameAssembly.dll did not load; only early modules can run.");
            return;
        }
        std::this_thread::sleep_for(kRuntimePoll);
    }
    logger_->Write("host", "GameAssembly.dll observed; waiting for IL2CPP to settle.");
    std::this_thread::sleep_for(kRuntimeSettle);

    resolver_ = std::make_unique<DynamicResolver>();
    std::string error;
    if (!resolver_->Initialize(error)) {
        logger_->Write("host", "IL2CPP exports unavailable: " + error);
        return;
    }
    const auto attach_deadline = std::chrono::steady_clock::now() + kRuntimeWait;
    while (!resolver_->AttachCurrentThread(error)) {
        if (std::chrono::steady_clock::now() >= attach_deadline || stop_requested_.load()) {
            logger_->Write("host", "IL2CPP thread attach failed: " + error);
            return;
        }
        std::this_thread::sleep_for(kRuntimePoll);
    }
    logger_->Write("host", "IL2CPP runtime ready.");
    modules_->SetRuntime(&api_, [this] { return resolver_->AttachOtherThread(); });
    modules_->PublishEvent("{\"type\":\"runtime_ready\"}");

    StartGameThread();
    resolver_->DetachCurrentThread();

    while (!stop_requested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        hooks_->Poll();
    }
}

void HostRuntime::StartGameThread() {
    if (!hooks_ || !hooks_->ChainApi()) return;
    // Classes are registered at startup, so a source that is missing now stays missing; a few
    // retries cover a hook broker that is momentarily busy.
    for (int attempt = 0; attempt < 5 && !stop_requested_.load(); ++attempt) {
        if (game_thread_->Start(api_, *hooks_->ChainApi())) return;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    logger_->Write("host.game_thread", "No frame source could be hooked; game-thread tasks will not run.");
}

void HostRuntime::RequestStop() { stop_requested_.store(true, std::memory_order_release); }

BE_Result HostRuntime::RetireHooks(const std::string& module_id) {
    return hooks_ ? hooks_->RetireModule(module_id) : BE_Result_NotReady;
}

void BE_CALL HostRuntime::LogCallback(void* context, const char* module_id, const char* message) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (runtime && runtime->logger_) runtime->logger_->Write(module_id ? module_id : "module", message ? message : "");
}

BE_Result BE_CALL HostRuntime::ResolveMethodCallback(void* context, const BE_MethodDescriptorV1* descriptor,
    BE_ResolvedMethodV1* result) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->resolver_ || !descriptor || !result) return BE_Result_NotReady;
    std::string error;
    const BE_Result status = runtime->resolver_->ResolveMethod(*descriptor, *result, error);
    if (status != BE_Result_Ok) runtime->logger_->Write("host.resolver", error);
    if (status == BE_Result_Ok && runtime->hooks_) runtime->hooks_->DescribeEntry(result->method_pointer, MethodLabel(*descriptor));
    return status;
}

BE_Result BE_CALL HostRuntime::ResolveFieldCallback(void* context, const BE_FieldDescriptorV1* descriptor,
    BE_ResolvedFieldV1* result) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->resolver_ || !descriptor || !result) return BE_Result_NotReady;
    std::string error;
    const BE_Result status = runtime->resolver_->ResolveField(*descriptor, *result, error);
    if (status != BE_Result_Ok) runtime->logger_->Write("host.resolver", error);
    return status;
}

BE_Result BE_CALL HostRuntime::CreateHookCallback(void* context, const char* module_id, void* target,
    void* detour, void** original) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->hooks_ || !module_id) return BE_Result_NotReady;
    return runtime->hooks_->Create(module_id, target, detour, original);
}

BE_Result BE_CALL HostRuntime::ReleaseModuleHooksCallback(void* context, const char* module_id) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !module_id) return BE_Result_InvalidArgument;
    return runtime->hooks_ ? runtime->hooks_->ReleaseModule(module_id) : BE_Result_NotReady;
}

int BE_CALL HostRuntime::CopyModuleConfigurationCallback(void* context, const char* module_id,
    char* destination, size_t destination_size) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->modules_ || !module_id) return 0;
    return CopyText(runtime->modules_->Configuration(module_id), destination, destination_size);
}

int BE_CALL HostRuntime::CopyCatalogRootCallback(void* context, char* destination, size_t destination_size) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime ? CopyText(runtime->data_root_utf8_, destination, destination_size) : 0;
}

int BE_CALL HostRuntime::CopyManagedStringCallback(void* context, const void* managed_string,
    char* destination, size_t destination_size) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->resolver_ || !destination || destination_size == 0) return 0;
    return runtime->resolver_->CopyManagedString(managed_string, destination, destination_size);
}

BE_Result BE_CALL HostRuntime::ResolveClassCallback(void* context, const char* assembly_name,
    const char* namespace_name, const char* class_name, BE_ResolvedClassV1* result) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (!runtime || !runtime->resolver_ || !assembly_name || !namespace_name || !class_name || !result) {
        return BE_Result_NotReady;
    }
    std::string error;
    const BE_Result status = runtime->resolver_->ResolveClass(assembly_name, namespace_name, class_name, *result, error);
    if (status != BE_Result_Ok) runtime->logger_->Write("host.resolver", error);
    return status;
}

void* BE_CALL HostRuntime::ObjectNewCallback(void* context, const void* class_info) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->ObjectNew(class_info) : nullptr;
}

void* BE_CALL HostRuntime::StringNewCallback(void* context, const char* utf8) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->StringNew(utf8) : nullptr;
}

void* BE_CALL HostRuntime::RuntimeInvokeCallback(void* context, const void* method_info, void* instance,
    void** parameters, void** exception) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->RuntimeInvoke(method_info, instance, parameters, exception)
                                         : nullptr;
}

void* BE_CALL HostRuntime::ObjectUnboxCallback(void* context, void* boxed_value) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->ObjectUnbox(boxed_value) : nullptr;
}

void* BE_CALL HostRuntime::FieldGetValueObjectCallback(void* context, const void* field_info, void* instance) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->FieldGetValueObject(field_info, instance) : nullptr;
}

uint32_t BE_CALL HostRuntime::GCHandleNewCallback(void* context, void* object, int pinned) {
    auto* runtime = static_cast<HostRuntime*>(context);
    return runtime && runtime->resolver_ ? runtime->resolver_->GCHandleNew(object, pinned != 0) : 0;
}

void BE_CALL HostRuntime::GCHandleFreeCallback(void* context, uint32_t handle) {
    auto* runtime = static_cast<HostRuntime*>(context);
    if (runtime && runtime->resolver_) runtime->resolver_->GCHandleFree(handle);
}

} // namespace EFML::Host
