#pragma once
#include "EFML/Module.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace EFML::Host {

class GameThread;

// Loads modules listed in modules\index.json, delivers their callbacks on one worker thread and
// serves the local RPC used by the manager and module pages. Derived from Better-Endfield's
// ThirdPartyHost: same index schema, manifest format, RPC endpoints and lifecycle rules.
//
// index.json: {"schema":1, "port":1024-65535, "token":"[A-Za-z0-9_-]{32,128}",
//   "modules":[{"id","directory","generation","enabled","configuration":{}}], ...}
// The index is re-read when it changes; port and token changes need a game restart.
class ModuleHost {
public:
    using Log = std::function<void(const std::string& source, const std::string& message)>;
    struct Options {
        std::filesystem::path index;
        std::string platform = "windows-x64";
        std::string data_root;    // UTF-8, reported to modules.
        Log log;
        const BE_HookChainApiV1* hooks = nullptr;
        GameThread* game_thread = nullptr;
    };

    ModuleHost();
    ~ModuleHost();

    bool Start(Options options);
    // Runtime helpers became available. attach_worker attaches the calling thread to IL2CPP;
    // the worker calls it before delivering further callbacks.
    void SetRuntime(const BE_HostApiV1* runtime, std::function<void*()> attach_worker);
    // Queues a host event ({"type":...}) for subscribed modules.
    void PublishEvent(const std::string& json);
    // The module's current JSON configuration ("{}" when unknown); any thread.
    std::string Configuration(const std::string& id) const;
    void Stop();

    // Creates an index with a fresh port and token when none exists.
    static bool EnsureIndex(const std::filesystem::path& index, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace EFML::Host
