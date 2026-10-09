// Test module built like a Better-Endfield 3.x module: only BetterEndfield_GetThirdPartyModuleV1
// is exported. Its first initialize returns NotReady to exercise the host's retry.
#include "BetterEndfield/ThirdPartyModule.h"

namespace {
const BE_ThirdPartyHostV1* host = nullptr;
int attempts = 0;

BE_Result BE_CALL Initialize(const BE_ThirdPartyHostV1* provided, const char*) {
    if (++attempts == 1) return BE_Result_NotReady;
    host = provided;
    host->log(host->context, "legacy module started");
    return BE_Result_Ok;
}
BE_Result BE_CALL Message(const char* request, const char*) {
    const auto* ext = EFML_GetExtension(host);
    const void* service = ext ? ext->get_service(host->context, "example.echo.counter", 1) : nullptr;
    return host->reply(host->context, request, BE_Result_Ok,
        service ? "{\"legacy\":true,\"service\":true}" : "{\"legacy\":true,\"service\":false}");
}
void BE_CALL Shutdown() { host = nullptr; }
const BE_ThirdPartyModuleV1 legacy{sizeof(BE_ThirdPartyModuleV1), 1, "test.legacy", Initialize, nullptr, Message, Shutdown};
} // namespace

BE_EXPORT const BE_ThirdPartyModuleV1* BE_CALL BetterEndfield_GetThirdPartyModuleV1() { return &legacy; }
