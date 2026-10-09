#pragma once
#include "EFML/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace EFML::Host {

class Logger;

// Runs module work on Unity's main thread. The Host hooks methods the game calls every frame
// (CameraManager.TailLateTick in the world, EventSystem.Update everywhere UGUI runs) through the
// shared hook chain, and uses UnityEngine.Time.frameCount so work runs once per frame whichever
// source fires first. Owners are module ids; removing an owner waits for a running frame.
class GameThread {
public:
    explicit GameThread(Logger& logger);

    // Resolves and hooks the frame sources; safe to call again until it succeeds.
    bool Start(const BE_HostApiV1& runtime, const BE_HookChainApiV1& hooks);
    bool Ready() const { return ready_.load(std::memory_order_acquire); }
    uint64_t Frames() const { return frames_.load(std::memory_order_relaxed); }
    bool IsGameThread() const;
    std::string SourceName() const;

    BE_Result Post(const std::string& owner, EFML_TaskFn task, void* user);
    BE_Result AddFrame(const std::string& owner, EFML_FrameFn callback, void* user, uint64_t* handle);
    BE_Result RemoveFrame(const std::string& owner, uint64_t handle);
    void RemoveOwner(const std::string& owner);

    // Called once, on the game thread, when the first frame arrives.
    std::function<void()> on_ready;

    // Entered by the frame-source detours.
    void Tick(int source);

private:
    struct Task { std::string owner; EFML_TaskFn task; void* user; };
    struct Frame { std::string owner; EFML_FrameFn callback; void* user; uint64_t handle; bool removed; };

    void RunTasks();
    void RunFrames(uint64_t frame);

    Logger& logger_;
    std::atomic_bool ready_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint32_t> thread_id_{0};
    std::atomic<int> source_{-1};
    int last_unity_frame_ = -1;
    int (*frame_count_)(const void*) = nullptr;
    const void* frame_count_method_ = nullptr;
    bool started_ = false;

    std::shared_mutex running_;   // Shared while a frame runs; exclusive to remove an owner.
    std::mutex queue_mutex_;
    std::vector<Task> tasks_;
    std::vector<Frame> frames_list_;
    std::vector<Frame> added_;
    uint64_t next_handle_ = 1;
};

} // namespace EFML::Host
