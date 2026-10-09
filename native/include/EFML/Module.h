#pragma once
// EFML module entry and host interface.
//
// A module is a DLL exporting EFML_GetModuleV1 (or, for modules built for Better-Endfield,
// BetterEndfieldNext_GetThirdPartyModuleV1 / BetterEndfield_GetThirdPartyModuleV1: EFML loads
// all three, they return the same structure). EFML passes BE_ThirdPartyHostV1 with
// struct_size == sizeof(EFML_HostV1); EFML_GetExtension() returns the EFML additions, or null
// when the module runs under a host without them.
//
// Threads: initialize, configuration_changed, on_message and shutdown run one at a time on the
// module worker, which EFML attaches to IL2CPP once the runtime is ready. Frame callbacks and
// game-thread tasks run on Unity's main thread. UTF-8 strings are borrowed for the call only.
// Loaded libraries stay resident until the game exits.
#include "ModuleApi.h"
#include "HookChain.h"

#define EFML_THIRD_PARTY_ABI_V1 1u
#define EFML_ENTRY_V1 "EFML_GetModuleV1"
#define EFML_BE_ENTRY_V1 "BetterEndfield_GetThirdPartyModuleV1"
#define EFML_BE_NEXT_ENTRY_V1 "BetterEndfieldNext_GetThirdPartyModuleV1"

typedef struct BE_ThirdPartyHostV1 {
    uint32_t struct_size;
    uint32_t version;   // 1
    void* context;
    const char* module_id;
    const char* platform;       // "windows-x64"
    const char* package_root;   // The module's installed directory (UTF-8).
    const BE_HostApiV1* runtime;     // IL2CPP helpers when ready at initialization, else null.
    const BE_HookChainApiV1* hooks;  // Shared hook chain, or null.
    void(BE_CALL* log)(void* context, const char* message);
    // Answers a page/RPC request received through on_message. json must be a JSON value.
    BE_Result(BE_CALL* reply)(void* context, const char* request_id, BE_Result result, const char* json);
    // Pushes an unsolicited JSON event to the module's page and RPC clients.
    BE_Result(BE_CALL* emit)(void* context, const char* json);
    // IL2CPP helpers, or null while the runtime is not ready.
    const BE_HostApiV1*(BE_CALL* get_runtime)(void* context);
} BE_ThirdPartyHostV1;

typedef struct BE_ThirdPartyModuleV1 {
    uint32_t struct_size;   // sizeof(BE_ThirdPartyModuleV1)
    uint32_t version;       // 1
    const char* id;         // Must equal the manifest id.
    BE_Result(BE_CALL* initialize)(const BE_ThirdPartyHostV1* host, const char* configuration_json);
    BE_Result(BE_CALL* configuration_changed)(const char* configuration_json);   // Optional.
    BE_Result(BE_CALL* on_message)(const char* request_id, const char* body_json);
    void(BE_CALL* shutdown)(void);   // Optional. Disabling a module calls it; it may be re-initialized later.
} BE_ThirdPartyModuleV1;

typedef const BE_ThirdPartyModuleV1*(BE_CALL* BE_GetThirdPartyModuleV1Fn)(void);

// ---- EFML additions -------------------------------------------------------------------------

#define EFML_EXTENSION_V1 1u

typedef void(BE_CALL* EFML_TaskFn)(void* user);
// frame: EFML's frame counter (increments once per rendered frame).
typedef void(BE_CALL* EFML_FrameFn)(void* user, uint64_t frame);
// json: {"type": "...", ...}; see docs/MODULE-API.md for the event list.
typedef void(BE_CALL* EFML_EventFn)(void* user, const char* json);

typedef struct EFML_HostExtV1 {
    uint32_t struct_size;
    uint32_t version;           // EFML_EXTENSION_V1
    const char* host_version;   // e.g. "1.0.0"
    const char* data_root;      // EFML data directory (UTF-8).

    // Game thread. Tasks run once, in order, at the start of the next frame; frame callbacks run
    // every frame until removed. Both stop automatically when the module is disabled. Until the
    // game thread is found (see the "game_thread" event) tasks are queued.
    BE_Result(BE_CALL* post_to_game_thread)(void* context, EFML_TaskFn task, void* user);
    BE_Result(BE_CALL* add_frame_callback)(void* context, EFML_FrameFn callback, void* user, uint64_t* handle);
    BE_Result(BE_CALL* remove_frame_callback)(void* context, uint64_t handle);
    int(BE_CALL* is_game_thread)(void* context);
    uint64_t(BE_CALL* frame_count)(void* context);

    // Services: named, versioned function tables shared between modules. A provider registers
    // during initialize; consumers list it in "dependencies" so it starts first. Registrations
    // are withdrawn when the provider is disabled.
    BE_Result(BE_CALL* register_service)(void* context, const char* name, uint32_t version, const void* api);
    const void*(BE_CALL* get_service)(void* context, const char* name, uint32_t min_version);

    // Host events, delivered on the module worker.
    BE_Result(BE_CALL* subscribe)(void* context, EFML_EventFn callback, void* user, uint64_t* handle);
    BE_Result(BE_CALL* unsubscribe)(void* context, uint64_t handle);
} EFML_HostExtV1;

typedef struct EFML_HostV1 {
    BE_ThirdPartyHostV1 base;
    const EFML_HostExtV1* efml;
} EFML_HostV1;

static inline const EFML_HostExtV1* EFML_GetExtension(const BE_ThirdPartyHostV1* host) {
    if (!host || host->struct_size < sizeof(EFML_HostV1)) return 0;
    const EFML_HostExtV1* ext = ((const EFML_HostV1*)host)->efml;
    return ext && ext->struct_size >= sizeof(EFML_HostExtV1) && ext->version >= EFML_EXTENSION_V1 ? ext : 0;
}

#if defined(__cplusplus)
#define EFML_EXTERN_C extern "C"
#else
#define EFML_EXTERN_C
#endif
#if defined(_WIN32)
#define EFML_MODULE_EXPORT EFML_EXTERN_C __declspec(dllexport)
#else
#define EFML_MODULE_EXPORT EFML_EXTERN_C __attribute__((visibility("default")))
#endif

// Declares the entry points for a module defined as `static const BE_ThirdPartyModuleV1 name`.
// The Better-Endfield names are exported too, so the same DLL also loads under Better-Endfield.
#define EFML_DEFINE_MODULE(name) \
    EFML_MODULE_EXPORT const BE_ThirdPartyModuleV1* BE_CALL EFML_GetModuleV1() { return &(name); } \
    EFML_MODULE_EXPORT const BE_ThirdPartyModuleV1* BE_CALL BetterEndfield_GetThirdPartyModuleV1() { return &(name); } \
    EFML_MODULE_EXPORT const BE_ThirdPartyModuleV1* BE_CALL BetterEndfieldNext_GetThirdPartyModuleV1() { return &(name); }
