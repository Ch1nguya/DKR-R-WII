// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Version Wii : le multijoueur en ligne n'existe pas (4 joueurs locaux, comportement N64 d'origine).
// Ce fichier remplace runtime_netplay.cpp et la session reseau (DirectSession) par des versions inertes : aucune
// session n'est jamais active, les effets de bord externes sont toujours autorises, la boucle de jeu n'est pas
// detournee. Le code reseau complet (netplay/*.cpp, GekkoNet, chiffrement) n'est pas lie dans le .dol.
#if defined(GEKKO)

#include "runtime_netplay.hpp"

extern "C" void dkr_custom_tracks_prepare_level(std::uint8_t*, recomp_context*);

namespace dkr::runtime::netplay {

DirectSession::DirectSession() = default;
DirectSession::~DirectSession() = default;
bool DirectSession::active() const { return false; }
SessionView DirectSession::view() const { return {}; }
RuntimeSessionView DirectSession::runtime_view() const { return {}; }
void DirectSession::fail_runtime_start(std::string) {}

namespace {
DirectSession g_session;
}

DirectSession& session() { return g_session; }
void bind_external_session(DirectSession*) {}
void reset_runtime_state() {}
void register_runtime_context(std::uint8_t*, recomp_context*) {}
void unregister_runtime_context(std::uint8_t*, recomp_context*) {}
void on_frame_boundary(std::uint8_t*, recomp_context*) {}
void prepare_controller_init() {}
void seed_character_select_ai(std::uint8_t*, recomp_context*) {}
void resolve_authored_input_frame(std::uint8_t*, recomp_context*) {}
std::uint32_t authored_frame() { return 0U; }
void begin_gameplay_level(std::uint8_t*, recomp_context*) {}
void complete_gameplay_level(std::uint8_t*, recomp_context*) {}
void end_gameplay_level() {}
void seal_race_finish(std::uint8_t*, std::uint8_t) {}
void commit_authoritative_gameplay_frame(std::uint8_t*) {}
int drive_authored_tick(std::uint8_t*, recomp_context*) { return 0; }
bool rollback_replay_active() { return false; }
bool external_side_effects_allowed() { return true; }
std::uint32_t authored_simulation_pacing_scale_milli() { return 1000U; }
OnlineWaitReason online_wait_reason() { return OnlineWaitReason::None; }
std::uint64_t online_wait_generation() { return 0U; }
std::uint64_t online_wait_episode() { return 0U; }
bool online_wait_active() { return false; }
bool physical_input_poll_allowed() { return true; }
void begin_presentation_random_scope() {}
void end_presentation_random_scope() {}
bool override_presentation_random_range(recomp_context*) { return false; }
RollbackMetrics rollback_metrics() { return {}; }

} // namespace dkr::runtime::netplay

extern "C" void dkr_netplay_character_select_enter(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_character_select_lock(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_character_select_ai_seed(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_gameplay_level_begin(std::uint8_t* rdram, recomp_context* context) {
    dkr_custom_tracks_prepare_level(rdram, context); // pistes personnalisees (hors ligne) : seule action conservee
}
extern "C" void dkr_netplay_gameplay_level_ready(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_gameplay_level_end(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_postrace_barrier(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_adventure_finish_barrier(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_prepare_controller_init(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_resolve_authored_input_frame(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_authoritative_frame_commit(std::uint8_t*, recomp_context*) {}
extern "C" int dkr_netplay_drive_authored_tick(std::uint8_t*, recomp_context*) { return 0; }
extern "C" int dkr_netplay_presentation_output_allowed() { return 1; }
extern "C" void dkr_netplay_presentation_random_begin(std::uint8_t*, recomp_context*) {}
extern "C" void dkr_netplay_presentation_random_end(std::uint8_t*, recomp_context*) {}
extern "C" int dkr_netplay_presentation_random_range(std::uint8_t*, recomp_context*) { return 0; }

#endif
