// Derived from Better-Endfield's third_party_host.cpp (https://github.com/Dr-hydra/Better-Endfield),
// AGPL-3.0. Rewritten for readability; adds runtime-gated loading, retries, the IL2CPP-attached
// worker and the EFML extension table (game thread, services, events).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include "module_host.h"

#include "game_thread.h"
#include "paths.h"
#include "version.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace EFML::Host {
namespace {

using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t kLimit = 1024 * 1024;
constexpr size_t kMaxModules = 128;
constexpr int kMaxRetries = 30;
constexpr auto kRetryDelay = std::chrono::seconds(2);
const char* const kEntries[] = {EFML_ENTRY_V1, EFML_BE_NEXT_ENTRY_V1, EFML_BE_ENTRY_V1};

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Require(bool(in), "Module metadata cannot be opened");
    const auto size = in.tellg();
    Require(size > 0 && static_cast<uint64_t>(size) <= kLimit, "Module metadata exceeds limit");
    std::string data(static_cast<size_t>(size), '\0');
    in.seekg(0);
    Require(bool(in.read(data.data(), static_cast<std::streamsize>(data.size()))), "Module metadata read failed");
    return data;
}

J ParseJson(const std::string& data) {
    Require(data.size() <= kLimit, "JSON exceeds limit");
    try {
        return J::parse(data);
    } catch (const J::parse_error&) {
        throw std::runtime_error("Invalid module JSON");
    }
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// [A-Za-z0-9][A-Za-z0-9_.-]{0,95}; host and Better-Endfield built-in prefixes are reserved.
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 96 || !std::isalnum(static_cast<unsigned char>(id[0]))) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') return false;
    }
    const auto lower = Lower(id);
    return !lower.starts_with("efml.") && !lower.starts_with("betterendfield.") &&
        !lower.starts_with("betterendfieldnext.") && lower != "voice.character";
}

// A file inside the package directory, rejecting anything that escapes it.
std::filesystem::path PackageFile(const std::filesystem::path& root, const std::string& relative) {
    Require(!relative.empty() && relative.find('\\') == std::string::npos &&
        relative.find(':') == std::string::npos && relative.find('\0') == std::string::npos,
        "Invalid package-relative path");
    const auto path = Utf8Path(relative);
    Require(!path.is_absolute(), "Absolute package path");
    for (const auto& part : path) Require(part != "." && part != ".." && !part.empty(), "Invalid package path segment");
    const auto base = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(base / path);
    const auto inside = candidate.lexically_relative(base);
    Require(!inside.empty() && !inside.is_absolute() && *inside.begin() != "..", "Path escapes the module package");
    Require(std::filesystem::is_regular_file(candidate), "Package file is missing");
    return candidate;
}

bool ConstantEqual(const std::string& a, const std::string& b) {
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        diff |= static_cast<size_t>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    }
    return diff == 0;
}

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned char c : data) {
        text.push_back(digits[c >> 4]);
        text.push_back(digits[c & 15]);
    }
    return text;
}

bool WriteAtomically(const std::filesystem::path& path, const std::string& text) {
    const auto temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string Truncated(const char* text, size_t limit) {
    size_t size = 0;
    while (size < limit && text[size]) ++size;
    return std::string(text, size);
}

} // namespace

struct ModuleHost::Impl {
    struct Module {
        Impl* owner = nullptr;
        std::string id, directory, generation, configuration = "{}";
        std::string status = "disabled", error, entry, name, version;
        bool requested = false, active = false, pinned = false, restart = false, early = false;
        int retries = 0;
        Clock::time_point retry_at{};
        void* library = nullptr;
        const BE_ThirdPartyModuleV1* api = nullptr;
        EFML_HostV1 host{};
        std::vector<std::string> dependencies;
        std::mutex events_mutex;
        std::deque<J> events;
        size_t event_bytes = 0;
        std::mutex logs_mutex;
        std::deque<std::string> logs;
    };
    struct Message { std::string module, request, body; };
    struct Service { std::string provider; uint32_t version; const void* api; };
    struct Subscription { std::string owner; EFML_EventFn callback; void* user; uint64_t handle; };

    Options options;
    std::string token, version = EFML_VERSION;
    uint16_t port = 0;
    SOCKET listener = INVALID_SOCKET;
    std::thread worker;
    std::atomic_bool stopping{false};
    std::filesystem::file_time_type stamp{};
    J last_index;

    std::mutex runtime_mutex;
    std::atomic<const BE_HostApiV1*> runtime{nullptr};
    std::function<void*()> attach_worker;
    bool runtime_seen = false, worker_attached = false;

    std::map<std::string, std::unique_ptr<Module>> modules;
    std::vector<std::string> order, loaded_order;
    std::deque<Message> inbox;
    size_t inbox_bytes = 0;

    std::mutex pending_mutex;
    std::deque<std::string> pending_events;
    std::mutex subscriptions_mutex;
    std::vector<Subscription> subscriptions;
    uint64_t next_subscription = 1;
    mutable std::mutex services_mutex;
    std::map<std::string, Service> services;
    mutable std::mutex config_mutex;
    std::map<std::string, std::string> configs;

    EFML_HostExtV1 ext{};

    Impl() {
        ext.struct_size = sizeof(EFML_HostExtV1);
        ext.version = EFML_EXTENSION_V1;
        ext.host_version = version.c_str();
        ext.post_to_game_thread = &PostToGameThread;
        ext.add_frame_callback = &AddFrameCallback;
        ext.remove_frame_callback = &RemoveFrameCallback;
        ext.is_game_thread = &IsGameThread;
        ext.frame_count = &FrameCount;
        ext.register_service = &RegisterService;
        ext.get_service = &GetService;
        ext.subscribe = &Subscribe;
        ext.unsubscribe = &Unsubscribe;
    }

    void Log(const std::string& source, const std::string& message) {
        if (options.log) options.log(source, message);
    }

    // ---- Callbacks given to modules ------------------------------------------------------

    static Module* Of(void* context) { return static_cast<Module*>(context); }

    static void BE_CALL LogMessage(void* context, const char* message) {
        auto* m = Of(context);
        if (!m || !message) return;
        const auto text = Truncated(message, 4096);
        {
            std::lock_guard lock(m->logs_mutex);
            if (m->logs.size() >= 64) m->logs.pop_front();
            m->logs.push_back(text);
        }
        m->owner->Log(m->id, text);
    }

    static const BE_HostApiV1* BE_CALL GetRuntime(void* context) {
        auto* m = Of(context);
        return m ? m->owner->runtime.load() : nullptr;
    }

    static void AddEvent(Module& module, J event) {
        const auto size = event.dump().size();
        if (size > kLimit) return;
        std::lock_guard lock(module.events_mutex);
        while (!module.events.empty() && (module.events.size() >= 256 || module.event_bytes + size > 2 * kLimit)) {
            module.event_bytes -= module.events.front().dump().size();
            module.events.pop_front();
        }
        module.event_bytes += size;
        module.events.push_back(std::move(event));
    }

    static BE_Result BE_CALL Reply(void* context, const char* request, BE_Result result, const char* json) {
        try {
            auto* m = Of(context);
            if (!m || !request || std::strlen(request) > 128 || !json) return BE_Result_InvalidArgument;
            AddEvent(*m, {{"kind", "reply"}, {"request_id", request}, {"result", result}, {"body", ParseJson(json)}});
            return BE_Result_Ok;
        } catch (...) {
            return BE_Result_InvalidArgument;
        }
    }

    static BE_Result BE_CALL Emit(void* context, const char* json) {
        try {
            auto* m = Of(context);
            if (!m || !json) return BE_Result_InvalidArgument;
            AddEvent(*m, {{"kind", "event"}, {"body", ParseJson(json)}});
            return BE_Result_Ok;
        } catch (...) {
            return BE_Result_InvalidArgument;
        }
    }

    static BE_Result BE_CALL PostToGameThread(void* context, EFML_TaskFn task, void* user) {
        auto* m = Of(context);
        if (!m || !m->owner->options.game_thread) return BE_Result_NotReady;
        return m->owner->options.game_thread->Post(m->id, task, user);
    }

    static BE_Result BE_CALL AddFrameCallback(void* context, EFML_FrameFn callback, void* user, uint64_t* handle) {
        auto* m = Of(context);
        if (!m || !m->owner->options.game_thread) return BE_Result_NotReady;
        return m->owner->options.game_thread->AddFrame(m->id, callback, user, handle);
    }

    static BE_Result BE_CALL RemoveFrameCallback(void* context, uint64_t handle) {
        auto* m = Of(context);
        if (!m || !m->owner->options.game_thread) return BE_Result_NotReady;
        return m->owner->options.game_thread->RemoveFrame(m->id, handle);
    }

    static int BE_CALL IsGameThread(void* context) {
        auto* m = Of(context);
        return m && m->owner->options.game_thread && m->owner->options.game_thread->IsGameThread() ? 1 : 0;
    }

    static uint64_t BE_CALL FrameCount(void* context) {
        auto* m = Of(context);
        return m && m->owner->options.game_thread ? m->owner->options.game_thread->Frames() : 0;
    }

    static BE_Result BE_CALL RegisterService(void* context, const char* name, uint32_t service_version, const void* api) {
        auto* m = Of(context);
        if (!m || !name || !*name || std::strlen(name) > 96 || !api) return BE_Result_InvalidArgument;
        auto& self = *m->owner;
        {
            std::lock_guard lock(self.services_mutex);
            auto found = self.services.find(name);
            if (found != self.services.end() && found->second.provider != m->id) return BE_Result_Conflict;
            self.services[name] = {m->id, service_version, api};
        }
        self.Log(m->id, std::string("Service registered: ") + name + " v" + std::to_string(service_version));
        self.PublishEvent(J{{"type", "service_registered"}, {"name", name}, {"version", service_version},
            {"provider", m->id}}.dump());
        return BE_Result_Ok;
    }

    static const void* BE_CALL GetService(void* context, const char* name, uint32_t min_version) {
        auto* m = Of(context);
        if (!m || !name) return nullptr;
        std::lock_guard lock(m->owner->services_mutex);
        const auto found = m->owner->services.find(name);
        return found != m->owner->services.end() && found->second.version >= min_version ? found->second.api : nullptr;
    }

    static BE_Result BE_CALL Subscribe(void* context, EFML_EventFn callback, void* user, uint64_t* handle) {
        auto* m = Of(context);
        if (!m || !callback) return BE_Result_InvalidArgument;
        std::lock_guard lock(m->owner->subscriptions_mutex);
        if (m->owner->subscriptions.size() >= 1024) return BE_Result_Failed;
        const uint64_t id = m->owner->next_subscription++;
        m->owner->subscriptions.push_back({m->id, callback, user, id});
        if (handle) *handle = id;
        return BE_Result_Ok;
    }

    static BE_Result BE_CALL Unsubscribe(void* context, uint64_t handle) {
        auto* m = Of(context);
        if (!m) return BE_Result_InvalidArgument;
        std::lock_guard lock(m->owner->subscriptions_mutex);
        const auto before = m->owner->subscriptions.size();
        std::erase_if(m->owner->subscriptions,
            [&](const Subscription& s) { return s.handle == handle && s.owner == m->id; });
        return before == m->owner->subscriptions.size() ? BE_Result_NotFound : BE_Result_Ok;
    }

    void PublishEvent(const std::string& json) {
        std::lock_guard lock(pending_mutex);
        if (pending_events.size() < 1024) pending_events.push_back(json);
    }

    void DispatchEvents() {
        std::deque<std::string> events;
        {
            std::lock_guard lock(pending_mutex);
            events.swap(pending_events);
        }
        for (const auto& json : events) {
            std::vector<Subscription> targets;
            {
                std::lock_guard lock(subscriptions_mutex);
                targets = subscriptions;
            }
            for (const auto& s : targets) {
                const auto found = modules.find(s.owner);
                if (found == modules.end() || !found->second->active) continue;
                try {
                    s.callback(s.user, json.c_str());
                } catch (...) {
                    Log(s.owner, "Event callback threw an exception.");
                }
            }
        }
    }

    // ---- Lifecycle -------------------------------------------------------------------------

    // Withdraws everything the module registered; the module's code may still be running on
    // the game thread until RemoveOwner returns.
    void Release(Module& m) {
        if (options.game_thread) options.game_thread->RemoveOwner(m.id);
        {
            std::lock_guard lock(subscriptions_mutex);
            std::erase_if(subscriptions, [&](const Subscription& s) { return s.owner == m.id; });
        }
        std::vector<std::string> withdrawn;
        {
            std::lock_guard lock(services_mutex);
            for (auto it = services.begin(); it != services.end();) {
                if (it->second.provider == m.id) {
                    withdrawn.push_back(it->first);
                    it = services.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& name : withdrawn) {
            PublishEvent(J{{"type", "service_withdrawn"}, {"name", name}, {"provider", m.id}}.dump());
        }
    }

    void CallShutdown(Module& m) {
        try {
            if (m.api && m.api->shutdown) m.api->shutdown();
        } catch (...) {
            m.error = "Module shutdown threw an exception";
        }
        if (options.hooks && options.hooks->disable_module) options.hooks->disable_module(options.hooks->context, m.id.c_str());
    }

    void Disable(Module& m) {
        if (m.active) {
            m.active = false;
            Release(m);
            CallShutdown(m);
            PublishEvent(J{{"type", "module_stopped"}, {"id", m.id}}.dump());
            Log(m.id, "Module stopped");
        }
        m.status = "disabled";
    }

    // Loads the library once (it stays pinned), then calls initialize.
    BE_Result Load(Module& m) {
        if (!m.library) {
            const auto manifest = ParseJson(ReadFile(Utf8Path(m.directory) / "module.json"));
            const auto library = PackageFile(Utf8Path(m.directory),
                manifest.at("libraries").at(options.platform).get<std::string>());
            Require(library.extension() == L".dll", "Windows module must be a DLL");
            m.library = LoadLibraryExW(library.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            Require(m.library, "Native module library failed to load");
            m.pinned = true;
            BE_GetThirdPartyModuleV1Fn entry = nullptr;
            for (const char* name : kEntries) {
                entry = reinterpret_cast<BE_GetThirdPartyModuleV1Fn>(GetProcAddress(static_cast<HMODULE>(m.library), name));
                if (entry) {
                    m.entry = name;
                    break;
                }
            }
            Require(entry, "Module entry point is missing (EFML_GetModuleV1)");
            const auto* candidate = entry();
            Require(candidate && candidate->struct_size >= sizeof(BE_ThirdPartyModuleV1) && candidate->version == 1 &&
                candidate->id && m.id == candidate->id && candidate->initialize && candidate->on_message,
                "Module ABI differs");
            m.api = candidate;
            m.host.base = {sizeof(EFML_HostV1), 1, &m, m.id.c_str(), options.platform.c_str(), m.directory.c_str(),
                runtime.load(), options.hooks, &LogMessage, &Reply, &Emit, &GetRuntime};
            m.host.efml = &ext;
        }
        Require(m.api != nullptr, "Rejected module library requires restart");
        m.host.base.runtime = runtime.load();
        BE_Result result = BE_Result_Failed;
        try {
            result = m.api->initialize(&m.host.base, m.configuration.c_str());
        } catch (...) {
            Release(m);
            CallShutdown(m);
            throw std::runtime_error("Module initialize threw an exception");
        }
        if (result != BE_Result_Ok) {
            Release(m);
            CallShutdown(m);
            return result;
        }
        m.active = true;
        m.status = "ready";
        m.error.clear();
        m.retries = 0;
        std::erase(loaded_order, m.id);
        loaded_order.push_back(m.id);
        LogMessage(&m, ("Module started via " + m.entry).c_str());
        PublishEvent(J{{"type", "module_ready"}, {"id", m.id}}.dump());
        return BE_Result_Ok;
    }

    bool Satisfied(const std::string& dependency) const {
        const auto found = modules.find(dependency);
        return found != modules.end() &&
            (found->second->active || (found->second->status == "ui_only" && found->second->requested));
    }

    // Applies an index snapshot to the module records.
    void ApplyIndex(const J& index, bool initial) {
        Require(index.is_object() && index.value("schema", 0) == 1 && index.at("modules").is_array() &&
            index.at("modules").size() <= kMaxModules, "Invalid module index");
        if (!initial) {
            Require(index.at("port") == port && index.at("token") == token,
                "Changing the RPC port or token requires a game restart");
        }
        // Validate everything first: a bad index changes nothing.
        std::set<std::string> present;
        for (const auto& record : index.at("modules")) {
            const auto id = record.at("id").get<std::string>();
            Require(ValidId(id) && present.insert(id).second, "Invalid or duplicate module ID");
            record.at("directory").get<std::string>();
            Require(record.value("configuration", J::object()).is_object(), "Module configuration must be an object");
        }
        order.clear();
        for (const auto& record : index.at("modules")) {
            const auto id = record.at("id").get<std::string>();
            auto& entry = modules[id];
            if (!entry) {
                entry = std::make_unique<Module>();
                entry->owner = this;
                entry->id = id;
            }
            auto& m = *entry;
            order.push_back(id);
            const auto directory = record.at("directory").get<std::string>();
            const auto generation = record.value("generation", directory);
            const auto config = record.value("configuration", J::object());
            Require(config.is_object(), "Module configuration must be an object");
            const auto encoded = config.dump();
            const bool enabled = record.value("enabled", false);
            if (m.pinned && (m.directory != directory || m.generation != generation)) {
                m.restart = true;
                m.error = "Installed module changed; restart the game to load it";
                m.requested = enabled;
                continue;
            }
            m.directory = directory;
            m.generation = generation;
            m.requested = enabled;
            if (m.status == "failed") m.status = "disabled";   // An index change retries failures.
            if (m.configuration != encoded) {
                m.configuration = encoded;
                {
                    std::lock_guard lock(config_mutex);
                    configs[id] = encoded;
                }
                if (m.active && enabled && m.api->configuration_changed &&
                    m.api->configuration_changed(encoded.c_str()) != BE_Result_Ok) {
                    m.error = "Configuration callback rejected update";
                }
            } else {
                std::lock_guard lock(config_mutex);
                configs[id] = encoded;
            }
        }
        for (auto& [id, m] : modules) {
            if (!present.contains(id)) {
                m->requested = false;
                m->restart = m->pinned;
            }
        }
    }

    // Stops what must stop and starts what can start.
    void Reconcile() {
        std::set<std::string> stop;
        for (const auto& [id, m] : modules) {
            if (m->active && !m->requested) stop.insert(id);
        }
        for (size_t pass = 0; pass < modules.size(); ++pass) {
            for (const auto& [id, m] : modules) {
                if (!m->active) continue;
                for (const auto& dep : m->dependencies) {
                    if (!Satisfied(dep) || stop.contains(dep)) stop.insert(id);
                }
            }
        }
        for (auto it = loaded_order.rbegin(); it != loaded_order.rend(); ++it) {
            if (!stop.contains(*it)) continue;
            auto& m = *modules.at(*it);
            Disable(m);
            if (m.requested) {
                m.status = "waiting_dependencies";
                m.error = "Module dependency is inactive";
            }
        }
        for (auto& [id, m] : modules) {
            if (!m->active && !m->requested && m->status != "failed") m->status = "disabled";
        }

        // Manifests are validated before any native code runs.
        const bool runtime_ready = runtime.load() != nullptr;
        const auto now = Clock::now();
        for (const auto& id : order) {
            auto& m = *modules.at(id);
            if (!m.requested || m.active || m.restart || m.status == "failed") continue;
            if (m.status == "retrying" && now < m.retry_at) continue;
            try {
                const auto manifest = ParseJson(ReadFile(Utf8Path(m.directory) / "module.json"));
                Require(manifest.value("format", 0) == 1 && manifest.value("abi", 0) == 1 && manifest.at("id") == id,
                    "Module identity or ABI differs");
                m.name = manifest.value("name", id);
                m.version = manifest.value("version", "");
                m.early = manifest.value("load", "") == "early";
                m.dependencies = manifest.value("dependencies", std::vector<std::string>{});
                Require(m.dependencies.size() <= kMaxModules, "Too many module dependencies");
                for (const auto& dependency : m.dependencies) {
                    Require(ValidId(dependency) && dependency != id, "Invalid module dependency");
                }
                if (!manifest.contains("libraries") || !manifest.at("libraries").contains(options.platform)) {
                    Require(manifest.contains("ui"), "Module has no library or page for this platform");
                    PackageFile(Utf8Path(m.directory), manifest.at("ui").get<std::string>());
                    m.status = "ui_only";
                    m.error.clear();
                    continue;
                }
                if (!m.early && !runtime_ready) {
                    m.status = "waiting_runtime";
                    m.error.clear();
                    continue;
                }
                if (m.status != "retrying") m.status = "waiting_dependencies";
                m.error.clear();
            } catch (const std::exception& error) {
                m.status = "failed";
                m.error = error.what();
            }
        }
        for (size_t pass = 0; pass < order.size(); ++pass) {
            bool progressed = false;
            for (const auto& id : order) {
                auto& m = *modules.at(id);
                if (!m.requested || m.active || m.restart) continue;
                if (m.status != "waiting_dependencies" && !(m.status == "retrying" && now >= m.retry_at)) continue;
                if (!std::all_of(m.dependencies.begin(), m.dependencies.end(),
                        [&](const std::string& dep) { return Satisfied(dep); })) {
                    m.status = "waiting_dependencies";
                    continue;
                }
                try {
                    const BE_Result result = Load(m);
                    if (result == BE_Result_Ok) {
                        progressed = true;
                    } else if (result == BE_Result_NotReady && ++m.retries <= kMaxRetries) {
                        m.status = "retrying";
                        m.error = "Module not ready yet; retrying";
                        m.retry_at = Clock::now() + kRetryDelay;
                    } else {
                        m.status = "failed";
                        m.error = "Module initialization returned " + std::to_string(static_cast<int>(result));
                    }
                } catch (const std::exception& error) {
                    m.status = "failed";
                    m.error = error.what();
                }
            }
            if (!progressed) break;
        }
        for (const auto& id : order) {
            auto& m = *modules.at(id);
            if (m.status == "waiting_dependencies" && m.requested && !m.active) {
                m.error = "Missing, disabled, failed or cyclic dependency";
            }
        }
    }

    bool RetryDue() const {
        const auto now = Clock::now();
        return std::any_of(modules.begin(), modules.end(), [&](const auto& item) {
            return item.second->status == "retrying" && now >= item.second->retry_at;
        });
    }

    // ---- RPC --------------------------------------------------------------------------------

    J Status() {
        J host{{"name", "EFML"}, {"version", version}, {"pid", GetCurrentProcessId()},
            {"data_root", options.data_root}, {"runtime_ready", runtime.load() != nullptr},
            {"worker_attached", worker_attached}};
        if (auto* game = options.game_thread) {
            host["game_thread"] = {{"ready", game->Ready()}, {"frames", game->Frames()}, {"source", game->SourceName()}};
        }
        J service_list = J::array();
        {
            std::lock_guard lock(services_mutex);
            for (const auto& [name, service] : services) {
                service_list.push_back({{"name", name}, {"version", service.version}, {"provider", service.provider}});
            }
        }
        host["services"] = service_list;
        J result{{"connected", true}, {"protocol", 1}, {"callback_thread", "worker"}, {"host", host}, {"modules", J::array()}};
        for (const auto& [id, m] : modules) {
            J logs = J::array();
            {
                std::lock_guard lock(m->logs_mutex);
                for (const auto& line : m->logs) logs.push_back(line);
            }
            result["modules"].push_back({{"id", id}, {"name", m->name}, {"version", m->version}, {"status", m->status},
                {"error", m->error}, {"enabled", m->active}, {"requested_enabled", m->requested},
                {"restart_required", m->restart}, {"entry", m->entry}, {"logs", logs}});
        }
        return result;
    }

    J Handle(const std::string& method, const std::string& path, const std::string& body) {
        if (method == "GET" && path == "/status") return Status();
        Require(method == "POST", "Unsupported request method");
        const auto request = ParseJson(body);
        const auto id = request.at("module_id").get<std::string>();
        const auto found = modules.find(id);
        Require(found != modules.end(), "Unknown module ID");
        auto& m = *found->second;
        if (path == "/poll") {
            J messages = J::array();
            std::lock_guard lock(m.events_mutex);
            while (!m.events.empty()) {
                messages.push_back(std::move(m.events.front()));
                m.events.pop_front();
            }
            m.event_bytes = 0;
            return {{"messages", messages}};
        }
        if (path == "/send") {
            Require(m.active, "Module is not active");
            const auto request_id = request.at("request_id").get<std::string>();
            Require(!request_id.empty() && request_id.size() <= 128 && request_id.find('\0') == std::string::npos,
                "Invalid request ID");
            const auto payload = request.at("body").dump();
            Require(inbox.size() < 256 && inbox_bytes + payload.size() <= 4 * kLimit, "Module message queue is full");
            inbox_bytes += payload.size();
            inbox.push_back({id, request_id, payload});
            return {{"accepted", true}, {"result", BE_Result_Ok}};
        }
        if (path == "/configure") {
            const auto config = request.at("configuration");
            Require(config.is_object(), "Configuration must be an object");
            const auto encoded = config.dump();
            m.configuration = encoded;
            {
                std::lock_guard lock(config_mutex);
                configs[id] = encoded;
            }
            BE_Result result = BE_Result_Ok;
            if (m.active && m.api->configuration_changed) result = m.api->configuration_changed(encoded.c_str());
            return {{"accepted", result == BE_Result_Ok}, {"result", result}};
        }
        throw std::runtime_error("Unknown endpoint");
    }

    void Client(SOCKET fd) {
        DWORD timeout = 2000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        int code = 200;
        J response;
        try {
            std::string data;
            char buffer[4096];
            size_t boundary = std::string::npos;
            while ((boundary = data.find("\r\n\r\n")) == std::string::npos) {
                const int count = recv(fd, buffer, sizeof(buffer), 0);
                Require(count > 0, "Incomplete HTTP headers");
                data.append(buffer, static_cast<size_t>(count));
                Require(data.size() <= 16384, "HTTP headers exceed limit");
            }
            const auto first = data.find("\r\n");
            const auto space = data.find(' ');
            const auto second = data.find(' ', space + 1);
            Require(first != std::string::npos && space < first && second < first, "Malformed HTTP request");
            const auto method = data.substr(0, space);
            const auto path = data.substr(space + 1, second - space - 1);
            std::map<std::string, std::string> headers;
            for (size_t start = first + 2; start < boundary;) {
                const auto end = data.find("\r\n", start);
                const auto colon = data.find(':', start);
                Require(colon < end, "Malformed HTTP header");
                auto key = Lower(data.substr(start, colon - start));
                auto value = data.substr(colon + 1, end - colon - 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                Require(headers.emplace(key, value).second, "Duplicate HTTP header");
                start = end + 2;
            }
            if (!ConstantEqual(headers["authorization"], "Bearer " + token)) {
                code = 401;
                response = {{"error", "Unauthorized"}};
            } else {
                Require(!headers.contains("transfer-encoding"), "Chunked requests are not supported");
                size_t length = 0;
                if (headers.contains("content-length")) {
                    const auto& value = headers.at("content-length");
                    Require(!value.empty() && value.size() < 10 &&
                        std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }),
                        "Invalid content length");
                    length = std::stoull(value);
                }
                Require(length <= kLimit, "Request body exceeds limit");
                const auto offset = boundary + 4;
                while (data.size() - offset < length) {
                    const int count = recv(fd, buffer, sizeof(buffer), 0);
                    Require(count > 0, "Incomplete request body");
                    data.append(buffer, static_cast<size_t>(count));
                    Require(data.size() - offset <= kLimit, "Request body exceeds limit");
                }
                response = Handle(method, path, data.substr(offset, length));
            }
        } catch (const std::exception& error) {
            code = 400;
            response = {{"error", error.what()}};
        } catch (...) {
            code = 500;
            response = {{"error", "Module callback failed"}};
        }
        const auto body = response.dump(-1, ' ', false, J::error_handler_t::replace);
        const auto output = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
            "\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close"
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < output.size()) {
            const int count = send(fd, output.data() + sent,
                static_cast<int>(std::min<size_t>(output.size() - sent, INT_MAX)), 0);
            if (count <= 0) break;
            sent += static_cast<size_t>(count);
        }
    }

    // ---- Worker ------------------------------------------------------------------------------

    void DeliverMessages() {
        while (!inbox.empty() && !stopping.load()) {
            auto message = std::move(inbox.front());
            inbox.pop_front();
            inbox_bytes -= message.body.size();
            const auto found = modules.find(message.module);
            if (found == modules.end()) continue;
            auto& m = *found->second;
            BE_Result result = BE_Result_NotReady;
            try {
                if (m.active) result = m.api->on_message(message.request.c_str(), message.body.c_str());
            } catch (...) {
                result = BE_Result_Failed;
            }
            if (result != BE_Result_Ok) Reply(&m, message.request.c_str(), result, "{\"error\":\"Module message was rejected\"}");
        }
    }

    void CheckRuntime() {
        if (runtime_seen || !runtime.load()) return;
        runtime_seen = true;
        std::function<void*()> attach;
        {
            std::lock_guard lock(runtime_mutex);
            attach = attach_worker;
        }
        worker_attached = attach && attach() != nullptr;
        Log("host.modules", worker_attached ? "Module worker attached to IL2CPP." : "Module worker could not attach to IL2CPP.");
        Reconcile();
    }

    void Loop() {
        try {
            ApplyIndex(last_index, true);
            Reconcile();
        } catch (const std::exception& error) {
            Log("host.modules", error.what());
        }
        auto next_check = Clock::now();
        while (!stopping.load()) {
            CheckRuntime();
            if (Clock::now() >= next_check) {
                next_check = Clock::now() + std::chrono::seconds(1);
                try {
                    const auto current = std::filesystem::last_write_time(options.index);
                    if (current != stamp) {
                        stamp = current;
                        last_index = ParseJson(ReadFile(options.index));
                        ApplyIndex(last_index, false);
                        Reconcile();
                    } else if (RetryDue()) {
                        Reconcile();
                    }
                } catch (const std::exception& error) {
                    Log("host.modules", error.what());
                }
            }
            DispatchEvents();
            DeliverMessages();
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval timeout{0, 100000};
            if (select(0, &set, nullptr, nullptr, &timeout) > 0) {
                const SOCKET client = accept(listener, nullptr, nullptr);
                if (client != INVALID_SOCKET) {
                    Client(client);
                    closesocket(client);
                }
            }
        }
        for (auto it = loaded_order.rbegin(); it != loaded_order.rend(); ++it) Disable(*modules.at(*it));
    }
};

ModuleHost::ModuleHost() : impl_(std::make_unique<Impl>()) {}
ModuleHost::~ModuleHost() { Stop(); }

bool ModuleHost::EnsureIndex(const std::filesystem::path& index, std::string& error) {
    std::error_code ignored;
    if (std::filesystem::is_regular_file(index, ignored)) return true;
    WSADATA ws{};
    WSAStartup(MAKEWORD(2, 2), &ws);
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    uint16_t port = 0;
    if (probe != INVALID_SOCKET && bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
        port = ntohs(address.sin_port);
    }
    if (probe != INVALID_SOCKET) closesocket(probe);
    const auto token = RandomHex(32);
    if (port < 1024 || token.empty()) {
        error = "Could not create the module index";
        return false;
    }
    std::filesystem::create_directories(index.parent_path(), ignored);
    const J initial{{"schema", 1}, {"port", port}, {"token", token}, {"modules", J::array()}};
    if (!WriteAtomically(index, initial.dump(2))) {
        error = "Could not write the module index";
        return false;
    }
    return true;
}

bool ModuleHost::Start(Options options) {
    auto& s = *impl_;
    if (s.worker.joinable()) return false;
    s.options = std::move(options);
    s.ext.data_root = s.options.data_root.c_str();
    try {
        std::string error;
        Require(EnsureIndex(s.options.index, error), "Module index unavailable");
        s.last_index = ParseJson(ReadFile(s.options.index));
        Require(s.last_index.value("schema", 0) == 1, "Unknown module index schema");
        const auto port = s.last_index.at("port").get<uint32_t>();
        Require(port >= 1024 && port <= 65535, "Invalid localhost port");
        s.port = static_cast<uint16_t>(port);
        s.token = s.last_index.at("token").get<std::string>();
        Require(s.token.size() >= 32 && s.token.size() <= 128 &&
            std::all_of(s.token.begin(), s.token.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }),
            "Invalid localhost credential");
        WSADATA ws{};
        Require(WSAStartup(MAKEWORD(2, 2), &ws) == 0, "Socket initialization failed");
        s.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(s.listener != INVALID_SOCKET, "Socket creation failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(s.port);
        Require(bind(s.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(s.listener, 8) == 0, "Localhost module port unavailable");
        s.stamp = std::filesystem::last_write_time(s.options.index);
        s.stopping = false;
        s.worker = std::thread([&s] { s.Loop(); });
        s.Log("host.modules", "Module RPC listening on 127.0.0.1:" + std::to_string(s.port));
        return true;
    } catch (const std::exception& error) {
        if (s.listener != INVALID_SOCKET) {
            closesocket(s.listener);
            s.listener = INVALID_SOCKET;
        }
        s.Log("host.modules", error.what());
        return false;
    }
}

void ModuleHost::SetRuntime(const BE_HostApiV1* runtime, std::function<void*()> attach_worker) {
    {
        std::lock_guard lock(impl_->runtime_mutex);
        impl_->attach_worker = std::move(attach_worker);
    }
    impl_->runtime.store(runtime);
}

void ModuleHost::PublishEvent(const std::string& json) { impl_->PublishEvent(json); }

std::string ModuleHost::Configuration(const std::string& id) const {
    std::lock_guard lock(impl_->config_mutex);
    const auto found = impl_->configs.find(id);
    return found == impl_->configs.end() ? "{}" : found->second;
}

void ModuleHost::Stop() {
    auto& s = *impl_;
    s.stopping = true;
    if (s.worker.joinable()) s.worker.join();
    if (s.listener != INVALID_SOCKET) {
        closesocket(s.listener);
        s.listener = INVALID_SOCKET;
    }
}

} // namespace EFML::Host
