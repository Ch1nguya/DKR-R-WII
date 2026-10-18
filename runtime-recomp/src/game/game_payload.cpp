#include "game_payload.hpp"

#include <atomic>

namespace dkr::runtime {

const GamePayload& payload_v77();
#ifndef DKR_WII_BUILD
const GamePayload& payload_v80();
#endif

namespace {

std::atomic<const GamePayload*> g_active_payload{nullptr};

} // namespace

const GamePayload* payload_for(rom::Revision revision) {
    switch (revision) {
    case rom::Revision::UsV77:
        return &payload_v77();
#ifndef DKR_WII_BUILD
    case rom::Revision::UsV80:
        return &payload_v80();
#endif
    default:
        return nullptr;
    }
}

bool select_payload(rom::Revision revision) {
    const GamePayload* payload = payload_for(revision);
    g_active_payload.store(payload, std::memory_order_release);
    return payload != nullptr;
}

const GamePayload* active_payload() {
    return g_active_payload.load(std::memory_order_acquire);
}

bool invoke_music_volume_set(std::uint8_t* rdram, recomp_context* context) {
    const GamePayload* payload = active_payload();
    if (payload == nullptr || payload->music_volume_set == nullptr) {
        return false;
    }
    payload->music_volume_set(rdram, context);
    return true;
}

bool invoke_main_game_loop(std::uint8_t* rdram, recomp_context* context) {
    const GamePayload* payload = active_payload();
    if (payload == nullptr || payload->main_game_loop == nullptr) {
        return false;
    }
    payload->main_game_loop(rdram, context);
    return true;
}

} // namespace dkr::runtime
