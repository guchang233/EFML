#pragma once
#include "ModuleApi.h"

#define EFML_HOOK_CHAIN_ABI_V1 1u

// Shared hook chain: every module hooking a target becomes a node of that target's chain.
// Nodes run in first-registration order; *next is a stable forwarding entry to the next node
// (ending at the original function). Disabled nodes become pass-throughs and are never freed,
// so a detour that is still running may keep calling its next.
typedef struct BE_HookChainApiV1 {
    uint32_t struct_size;
    uint32_t version;
    void* context;
    BE_Result(BE_CALL* create)(void* context, const char* module_id, void* target, void* detour,
        void** next, uint64_t* handle);
    BE_Result(BE_CALL* disable)(void* context, uint64_t handle);
    BE_Result(BE_CALL* disable_module)(void* context, const char* module_id);
} BE_HookChainApiV1;
