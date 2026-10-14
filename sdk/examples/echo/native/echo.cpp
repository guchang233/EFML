// Echo: the smallest complete EFML module. It answers page messages, emits events, and shows the
// EFML additions: a per-frame counter on the game thread, a service, and host events.
// Derived from Better-Endfield's Echo sample (AGPL-3.0).
#include "EFML/Module.h"

#include <atomic>
#include <string>

namespace {

const BE_ThirdPartyHostV1* host = nullptr;
const EFML_HostExtV1* efml = nullptr;   // Null when running under Better-Endfield.
std::string configuration = "{}";
uint64_t count = 0;
uint64_t frame_handle = 0;
std::atomic<uint64_t> frames_seen{0};
std::atomic<int> shutdown_attempts{0};

// A service other modules can fetch with get_service("example.echo.counter", 1).
struct EchoCounterV1 {
    uint64_t (*count)();
};
const EchoCounterV1 counter_service{[] { return count; }};

void BE_CALL OnFrame(void*, uint64_t) {
    frames_seen.fetch_add(1, std::memory_order_relaxed);   // Runs on the game thread.
}

void BE_CALL OnHostEvent(void*, const char* json) {
    const std::string event = std::string("{\"type\":\"host_event\",\"event\":") + json + "}";
    host->emit(host->context, event.c_str());
}

BE_Result BE_CALL Initialize(const BE_ThirdPartyHostV1* provided, const char* json) {
    if (!provided || provided->version != 1 || provided->struct_size < sizeof(BE_ThirdPartyHostV1) ||
        !provided->reply || !provided->emit) {
        return BE_Result_ContractMismatch;
    }
    host = provided;
    efml = EFML_GetExtension(provided);
    configuration = json ? json : "{}";
    count = 0;
    shutdown_attempts = 0;
    if (efml) {
        efml->add_frame_callback(host->context, &OnFrame, nullptr, &frame_handle);
        efml->register_service(host->context, "example.echo.counter", 1, &counter_service);
        efml->subscribe(host->context, &OnHostEvent, nullptr, nullptr);
    }
    host->log(host->context, efml ? "Echo initialized (EFML extensions available)" : "Echo initialized");
    return host->emit(host->context, "{\"type\":\"ready\"}");
}

BE_Result BE_CALL Configure(const char* json) {
    configuration = json ? json : "{}";
    return BE_Result_Ok;
}

BE_Result BE_CALL Message(const char* request, const char* body) {
    if (!host || !request || !body) return BE_Result_NotReady;
    ++count;
    const bool runtime = host->get_runtime && host->get_runtime(host->context);
    const std::string reply = "{\"counter\":" + std::to_string(count) + ",\"echo\":" + body +
        ",\"configuration\":" + configuration + ",\"runtime_ready\":" + (runtime ? "true" : "false") +
        ",\"efml\":" + (efml ? "true" : "false") +
        ",\"frames_seen\":" + std::to_string(frames_seen.load()) +
        ",\"host_frames\":" + std::to_string(efml ? efml->frame_count(host->context) : 0) + "}";
    const BE_Result result = host->reply(host->context, request, BE_Result_Ok, reply.c_str());
    const std::string event = "{\"type\":\"counter\",\"value\":" + std::to_string(count) + "}";
    host->emit(host->context, event.c_str());
    return result;
}

// Frame callbacks, services and subscriptions are withdrawn by EFML before this runs.
void BE_CALL Shutdown() {
    host = nullptr;
    efml = nullptr;
}

BE_Result BE_CALL PrepareShutdown() {
    const int attempt = ++shutdown_attempts;
    return configuration.find("\"shutdown_wait\":3") != std::string::npos && attempt < 3
        ? BE_Result_NotReady : BE_Result_Ok;
}

const EFML_ModuleWithShutdownBarrierV1 echo_module{
    {sizeof(EFML_ModuleWithShutdownBarrierV1), 1, "example.echo", Initialize, Configure, Message, Shutdown},
    PrepareShutdown};

} // namespace

EFML_DEFINE_MODULE(echo_module.base)
