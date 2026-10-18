// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Profileur d'echantillonnage des fonctions recompilees (build optionnel DKR_PROFILE_FUNCS=ON, voir docs/wii/DEBUGGING.md §21/§25).
// -finstrument-functions n'est applique qu'aux fonctions generees : chaque entree note "fonction courante". Un echantillonneur
// d'alarme (null_renderer.cpp, 1 kHz, contexte d'interruption) appelle dkr_funcprof_tick(thread) qui incremente un compteur par (thread, fonction) ;
// un thread ecrit le tout dans sd:/apps/DKR-R/funcprof.txt toutes les 10 s (adresses hote a resoudre avec nm).
// Limite : pas de hook de sortie utile, le temps passe dans l'appelant apres un retour est attribue a la derniere fonction entree.
#if defined(GEKKO) && defined(DKR_PROFILE_FUNCS)
#include <gccore.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>

extern "C" const char* dkr_thread_name(unsigned slot);

extern "C" {
void* volatile g_prof_cur_fn = nullptr;
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void*) { g_prof_cur_fn = fn; }
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void*, void*) {}
}

namespace {
constexpr unsigned kSlots = 8192;
void* g_key[kSlots];
unsigned g_slot[kSlots];
unsigned g_count[kSlots];
volatile unsigned g_total = 0;
lwp_t g_thread = 0;
bool g_started = false;

void Dump() {
    FILE* f = std::fopen("sd:/apps/DKR-R/funcprof.txt", "w");
    if (f == nullptr) return;
    std::fprintf(f, "total=%u\n", g_total);
    static bool used[kSlots];
    for (unsigned i = 0; i < kSlots; ++i) used[i] = false;
    for (int rank = 0; rank < 150; ++rank) {
        unsigned best = 0, bi = kSlots;
        for (unsigned i = 0; i < kSlots; ++i) {
            if (!used[i] && g_key[i] != nullptr && g_count[i] > best) { best = g_count[i]; bi = i; }
        }
        if (bi == kSlots) break;
        used[bi] = true;
        std::fprintf(f, "%p %s %u\n", g_key[bi], dkr_thread_name(g_slot[bi]), best);
    }
    std::fclose(f);
}

void* DumperMain(void*) {
    for (;;) {
        usleep(10000000);
        Dump();
    }
    return nullptr;
}
} // namespace

extern "C" void dkr_funcprof_tick(unsigned slot) {   // appele depuis l'alarme (interruption) : accès mémoire simples uniquement
    void* fn = g_prof_cur_fn;
    if (fn == nullptr) return;
    unsigned h = static_cast<unsigned>(((reinterpret_cast<std::uintptr_t>(fn) >> 2) + slot * 40503U) * 2654435761U) % kSlots;
    for (unsigned probe = 0; probe < 64; ++probe) {
        const unsigned i = (h + probe) % kSlots;
        if (g_key[i] == nullptr) { g_key[i] = fn; g_slot[i] = slot; g_count[i] = 1; break; }
        if (g_key[i] == fn && g_slot[i] == slot) { ++g_count[i]; break; }
    }
    ++g_total;
}

extern "C" void dkr_funcprof_start() {
    if (g_started) return;
    g_started = true;
    static u8 stack[16384] __attribute__((aligned(32)));
    LWP_CreateThread(&g_thread, DumperMain, nullptr, stack, sizeof(stack), 100);
}
#endif
