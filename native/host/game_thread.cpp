#include "game_thread.h"

#include "logging.h"

#include <Windows.h>

#include <algorithm>

namespace EFML::Host {
namespace {

GameThread* g_instance = nullptr;

using TickWithFloat = void(*)(void*, float, const void*);
using TickNoArgs = void(*)(void*, const void*);
TickWithFloat g_next_camera = nullptr;
TickNoArgs g_next_event_system = nullptr;

// Original first: modules see the frame after the game's own update for that source.
void CameraTickDetour(void* self, float delta, const void* method) {
    g_next_camera(self, delta, method);
    if (g_instance) g_instance->Tick(0);
}
void EventSystemDetour(void* self, const void* method) {
    g_next_event_system(self, method);
    if (g_instance) g_instance->Tick(1);
}

struct Source {
    const char* name;
    BE_MethodDescriptorV1 method;
    void* detour;
    void** next;
};

Source g_sources[] = {
    {"CameraManager.TailLateTick",
        {"Gameplay.Beyond.dll", "Beyond.Gameplay.View", "CameraManager", "TailLateTick",
            "System.Single", "System.Void", 1},
        reinterpret_cast<void*>(&CameraTickDetour), reinterpret_cast<void**>(&g_next_camera)},
    {"EventSystem.Update",
        {"UnityEngine.UI.dll", "UnityEngine.EventSystems", "EventSystem", "Update",
            nullptr, "System.Void", 0},
        reinterpret_cast<void*>(&EventSystemDetour), reinterpret_cast<void**>(&g_next_event_system)},
};

} // namespace

GameThread::GameThread(Logger& logger) : logger_(logger) {}

bool GameThread::Start(const BE_HostApiV1& runtime, const BE_HookChainApiV1& hooks) {
    if (started_) return true;
    g_instance = this;
    if (!frame_count_) {
        BE_MethodDescriptorV1 frame_count{"UnityEngine.CoreModule.dll", "UnityEngine", "Time",
            "get_frameCount", nullptr, "System.Int32", 0};
        BE_ResolvedMethodV1 resolved{};
        if (runtime.resolve_method(runtime.context, &frame_count, &resolved) == BE_Result_Ok &&
            resolved.method_pointer) {
            frame_count_ = reinterpret_cast<int (*)(const void*)>(resolved.method_pointer);
            frame_count_method_ = resolved.method_info;
        } else {
            logger_.Write("host.game_thread", "Time.frameCount unavailable; using the first frame source only.");
        }
    }
    int hooked = 0;
    for (auto& source : g_sources) {
        if (*source.next) { ++hooked; continue; }
        BE_ResolvedMethodV1 resolved{};
        if (runtime.resolve_method(runtime.context, &source.method, &resolved) != BE_Result_Ok ||
            !resolved.method_pointer) {
            logger_.Write("host.game_thread", std::string("Frame source not found: ") + source.name);
            continue;
        }
        uint64_t handle = 0;
        const BE_Result result = hooks.create(hooks.context, "efml.host", resolved.method_pointer,
            source.detour, source.next, &handle);
        if (result == BE_Result_Ok) {
            ++hooked;
            logger_.Write("host.game_thread", std::string("Frame source hooked: ") + source.name);
        } else {
            logger_.Write("host.game_thread", std::string("Frame source hook failed: ") + source.name +
                " (" + std::to_string(static_cast<int>(result)) + ")");
        }
    }
    started_ = hooked > 0;
    return started_;
}

bool GameThread::IsGameThread() const {
    const uint32_t id = thread_id_.load(std::memory_order_acquire);
    return id != 0 && id == GetCurrentThreadId();
}

std::string GameThread::SourceName() const {
    const int source = source_.load(std::memory_order_relaxed);
    return source >= 0 ? g_sources[source].name : "";
}

void GameThread::Tick(int source) {
    thread_local bool inside = false;
    if (inside) return;
    if (frame_count_) {
        const int frame = frame_count_(frame_count_method_);
        if (frame == last_unity_frame_) return;
        last_unity_frame_ = frame;
        int expected = -1;
        source_.compare_exchange_strong(expected, source);
    } else {
        int expected = -1;
        if (!source_.compare_exchange_strong(expected, source) && expected != source) return;
    }
    inside = true;
    const uint64_t frame = frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ready_.load(std::memory_order_acquire)) {
        thread_id_.store(GetCurrentThreadId(), std::memory_order_release);
        ready_.store(true, std::memory_order_release);
        logger_.Write("host.game_thread", "Game thread ready (" + SourceName() + ").");
        if (on_ready) on_ready();
    }
    {
        std::shared_lock running(running_);
        RunTasks();
        RunFrames(frame);
    }
    inside = false;
}

void GameThread::RunTasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard lock(queue_mutex_);
        tasks.swap(tasks_);
    }
    for (const auto& task : tasks) {
        try {
            task.task(task.user);
        } catch (...) {
            logger_.Write(task.owner, "Game-thread task threw an exception.");
        }
    }
}

void GameThread::RunFrames(uint64_t frame) {
    std::vector<Frame> frames;
    {
        std::lock_guard lock(queue_mutex_);
        for (auto& added : added_) {
            if (!added.removed) frames_list_.push_back(added);
        }
        added_.clear();
        std::erase_if(frames_list_, [](const Frame& entry) { return entry.removed; });
        frames = frames_list_;
    }
    for (const auto& entry : frames) {
        try {
            entry.callback(entry.user, frame);
        } catch (...) {
            logger_.Write(entry.owner, "Frame callback threw an exception and was removed.");
            std::lock_guard lock(queue_mutex_);
            for (auto& current : frames_list_) {
                if (current.handle == entry.handle) current.removed = true;
            }
        }
    }
}

BE_Result GameThread::Post(const std::string& owner, EFML_TaskFn task, void* user) {
    if (!task) return BE_Result_InvalidArgument;
    std::lock_guard lock(queue_mutex_);
    if (tasks_.size() >= 4096) return BE_Result_Failed;
    tasks_.push_back({owner, task, user});
    return BE_Result_Ok;
}

BE_Result GameThread::AddFrame(const std::string& owner, EFML_FrameFn callback, void* user,
    uint64_t* handle) {
    if (!callback) return BE_Result_InvalidArgument;
    std::lock_guard lock(queue_mutex_);
    if (frames_list_.size() + added_.size() >= 1024) return BE_Result_Failed;
    const uint64_t id = next_handle_++;
    added_.push_back({owner, callback, user, id, false});
    if (handle) *handle = id;
    return BE_Result_Ok;
}

BE_Result GameThread::RemoveFrame(const std::string& owner, uint64_t handle) {
    std::lock_guard lock(queue_mutex_);
    bool found = false;
    for (auto* list : {&frames_list_, &added_}) {
        for (auto& entry : *list) {
            if (entry.handle == handle && entry.owner == owner && !entry.removed) {
                entry.removed = true;
                found = true;
            }
        }
    }
    return found ? BE_Result_Ok : BE_Result_NotFound;
}

void GameThread::RemoveOwner(const std::string& owner) {
    auto erase = [&] {
        std::lock_guard lock(queue_mutex_);
        std::erase_if(tasks_, [&](const Task& task) { return task.owner == owner; });
        for (auto* list : {&frames_list_, &added_}) {
            for (auto& entry : *list) {
                if (entry.owner == owner) entry.removed = true;
            }
        }
    };
    if (IsGameThread()) {
        erase();   // Cannot wait for the frame this thread is running.
        return;
    }
    std::unique_lock running(running_);
    erase();
}

} // namespace EFML::Host
