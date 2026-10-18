#include "null_renderer.hpp"

#include "game_registration.hpp"
#include "revision_addresses.hpp"

#include "librecomp/game.hpp"

#include <cstdio>
#include <memory>

#if defined(GEKKO)
extern "C" float dkr_wii_cfg(const char* key, float def);
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <cstring>
#include <vector>
#include <cstdarg>
#include <unordered_map>
#include <unistd.h>
extern void* g_wii_xfb;
extern GXRModeObj* g_wii_rmode;
static void* g_gx_fifo = nullptr;
static bool g_pillarbox = false; // image 4:3 centree (bandes noires) sur une console en 16:9 : cfg aspect = 0 auto, 1 etire, 2 force les bandes
static u16 g_view_x = 0, g_view_w = 640;
static int g_diag_mode = 0; // cfg diag : 0 = aucun, 1 = console de diagnostic (bande noire de 128 lignes en haut), 2 = petit overlay FPS
// Journal de diagnostic en RAM (ecrit dans sd:/apps/DKR-R/debug.txt par dkr_dump_flush : HOME ou cfg dumpflush) ; evite des milliers d'ouvertures de fichiers pendant le jeu.
static char g_dbg_buf[512 * 1024];
static size_t g_dbg_pos = 0;
// Anneau des derniers evenements de dessin d'objets (G_VTX et G_TRIN) pour diagnostiquer un defaut visible a un instant precis :
// ecrit dans debug.txt par dkr_dump_flush (combinaison de touches L + R + Start sur la manette GameCube, voir update_screen).
constexpr u32 kRingN = 24576, kRingW = 128;
static char g_ring_name[40] = "sd:/apps/DKR-R/ring.txt";
static char* g_ring = nullptr;   // allouee a la demande (ring=1) : kRingN lignes de kRingW octets
static u32 g_ring_n = 0;
static void RingLog(const char* fmt, ...) {
    if (g_ring == nullptr) g_ring = static_cast<char*>(std::calloc(kRingN, kRingW));
    if (g_ring == nullptr) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_ring + static_cast<size_t>(g_ring_n % kRingN) * kRingW, kRingW, fmt, ap);
    va_end(ap);
    ++g_ring_n;
}
static void DbgLog(const char* fmt, ...) {
    if (g_dbg_pos + 512 >= sizeof(g_dbg_buf)) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(g_dbg_buf + g_dbg_pos, 512, fmt, ap);
    va_end(ap);
    if (n > 0) g_dbg_pos += static_cast<size_t>(n < 511 ? n : 511);
}
static int g_top_pad = 0;   // lignes de l'XFB reservees a la console (128 si diag=1, sinon l'image occupe tout l'ecran)
extern "C" void dkr_wii_draw_overlay(void* xfb, int width, int x, int y, const char* text);
#define DEFAULT_FIFO_SIZE (256 * 1024)
#endif

dkr::runtime::DiagnosticRenderer::DiagnosticRenderer() {
    setup_result = ultramodern::renderer::SetupResult::Success;
    chosen_api = ultramodern::renderer::GraphicsApi::Auto;

#if defined(GEKKO)
    if (g_wii_rmode != nullptr && g_gx_fifo == nullptr) {
        g_diag_mode = static_cast<int>(dkr_wii_cfg("diag", 0.0F));
        g_top_pad = g_diag_mode == 1 ? 128 : 0;
        g_gx_fifo = MEM_K0_TO_K1(memalign(32, DEFAULT_FIFO_SIZE));
        memset(g_gx_fifo, 0, DEFAULT_FIFO_SIZE);

        GX_Init(g_gx_fifo, DEFAULT_FIFO_SIZE);

        {   // Le jeu est en 4:3 : sur une console reglee en 16:9 l'image de 640 pixels est etiree par le televiseur ; on la limite aux 3/4 centraux.
            const int aspect = static_cast<int>(dkr_wii_cfg("aspect", 0.0F));
            g_pillarbox = aspect == 2 || (aspect == 0 && CONF_GetAspectRatio() == CONF_ASPECT_16_9);
            g_view_w = g_pillarbox ? static_cast<u16>(g_wii_rmode->fbWidth * 3 / 4) & ~1U : g_wii_rmode->fbWidth;
            g_view_x = static_cast<u16>((g_wii_rmode->fbWidth - g_view_w) / 2);
        }
        GXColor background = g_pillarbox ? GXColor{0x00, 0x00, 0x00, 0xFF} : GXColor{0x00, 0x1A, 0x33, 0xFF};
        GX_SetCopyClear(background, 0x00FFFFFF);

        GX_SetViewport(g_view_x, 0, g_view_w, g_wii_rmode->efbHeight - g_top_pad, 0, 1);
        f32 yscale = GX_GetYScaleFactor(g_wii_rmode->efbHeight, g_wii_rmode->xfbHeight);
        u32 xfbHeight = GX_SetDispCopyYScale(yscale);
        GX_SetScissor(g_view_x, 0, g_view_w, g_wii_rmode->efbHeight - g_top_pad);
        // Les 128 premières lignes de l'XFB sont réservées à la console de diagnostic
        GX_SetDispCopySrc(0, 0, g_wii_rmode->fbWidth, g_wii_rmode->efbHeight - g_top_pad);
        GX_SetDispCopyDst(g_wii_rmode->fbWidth, xfbHeight);
        {   // cfg smooth : 0 = sans filtre vertical, 1 = filtre du mode video (defaut), 2 = filtre renforce
            const int smooth = static_cast<int>(dkr_wii_cfg("smooth", 1.0F));
            static u8 kOff[7] = {0, 0, 0, 63, 0, 0, 0};
            static u8 kStrong[7] = {4, 8, 12, 16, 12, 8, 4};
            // L'anticrenelage materiel GX (MSAA) exige GX_PF_RGB565_Z16 et une image de travail de 264 lignes : le Z16 ne suffit pas pour la plage
            // de profondeur utilisee ici (z-fighting) ; l'option « Ameliore » n'active donc que les mipmaps et le filtrage anisotrope (wii_textures.inc).
            GX_SetCopyFilter(g_wii_rmode->aa, g_wii_rmode->sample_pattern, GX_TRUE,
                             smooth == 0 ? kOff : (smooth >= 2 ? kStrong : g_wii_rmode->vfilter));
        }
        GX_SetFieldMode(g_wii_rmode->field_rendering, ((g_wii_rmode->viHeight == 2 * g_wii_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

        GX_SetCullMode(GX_CULL_NONE);
        // (Pas de GX_CopyDisp ici : l'EFB n'est pas initialise apres GX_Init sur console reelle ; le copier dans l'XFB affichait du bruit
        //  jusqu'au premier vrai dessin du jeu. L'XFB reste noir, efface au demarrage.)
        GX_SetDispCopyGamma(GX_GM_1_0);

        // Configuration minimale des sommets GX : Position (x,y,z) + Couleur (r,g,b,a)
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);

        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0); // brouillard : alpha = facteur par sommet
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

        // Matrice de projection orthogonale directe sans dépendance à gu.o
        Mtx44 proj;
        std::memset(proj, 0, sizeof(proj));
        proj[0][0] = 1.0F; // scale X
        proj[1][1] = 1.0F; // scale Y
        proj[2][2] = 1.0F; // z déjà en [-1, 0] (fourni par le décodeur)
        proj[3][3] = 1.0F;
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

        Mtx modelview;
        std::memset(modelview, 0, sizeof(modelview));
        modelview[0][0] = 1.0F;
        modelview[1][1] = 1.0F;
        modelview[2][2] = 1.0F;
        GX_LoadPosMtxImm(modelview, GX_PNMTX0);

        GX_SetNumChans(1);
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

        GX_SetZCompLoc(GX_FALSE); // test Z après le test alpha : les texels transparents (feuillage) ne doivent pas écrire la profondeur
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

        std::fprintf(stderr, "[boot][gfx] Wii ATI Hollywood 3D GX initialized\n");
    }
#endif
}

bool dkr::runtime::DiagnosticRenderer::valid() {
    return true;
}

bool dkr::runtime::DiagnosticRenderer::update_config(
    const ultramodern::renderer::GraphicsConfig&,
    const ultramodern::renderer::GraphicsConfig&) {
    return false;
}

void dkr::runtime::DiagnosticRenderer::enable_instant_present() {}

#if defined(GEKKO)
#include "wii_renderer_state.inc"
#include "wii_renderer_cfg.inc"
#include "wii_renderer_rdp_state.inc"
#include "wii_textures.inc"

#include "wii_renderer_debug.inc"
#include "wii_renderer_geometry.inc"

#include "wii_renderer_dlparse.inc"
#endif

void dkr::runtime::DiagnosticRenderer::send_dl(const OSTask* task, std::uint8_t*) {
    const auto index = ++display_list_count_;
    if (index <= 5 || index % 120 == 0) {
        std::fprintf(stderr,
                     "[boot][gfx] display-list=%llu ucode=0x%08X data=0x%08X size=%u\n",
                     static_cast<unsigned long long>(index), task->t.ucode,
                     task->t.data_ptr, task->t.data_size);
    }

#if defined(GEKKO)
    if (rdram_ == nullptr || task->t.data_ptr == 0) return;
    LoadRenderCfg();
    g_dl_processed++;
    if (g_cfg_skip > 0 && (g_dl_processed % (static_cast<u32>(g_cfg_skip) + 1U)) != 0) return; // saute le dessin (option skip=N)

    // Décodage des commandes Display List F3DDKR
    const u32 dl_offset = task->t.data_ptr & 0x00FFFFFF;
    const u32* cmd_ptr = reinterpret_cast<const u32*>(rdram_ + dl_offset);
    // data_size est un nombre de commandes Gfx (gfxtask_run_xbus : dlEnd - dlBegin) ou un nombre d'octets selon la variante
    // de tâche ; dans les deux cas c'est une borne sûre, la liste se termine de toute façon par G_ENDDL.
    const u32 cmd_count = task->t.data_size;

    auto reset_state = [&]() {
        g_dl_budget = 50000;
        g_dl_budget_exhausted = 0;
        g_dl_depth_exceeded = 0;
        g_dmadl_count = 0;
        g_dmadl_max_count = 0;
        g_dmadl_rejected = 0;
        g_gdl_rejected = 0;
        g_last_bad_gdl_w1 = 0;
        g_last_bad_dmadl_w1 = 0;
        g_cur_mvp = 0;
        g_billboard = false;
        g_vtx_count = 0;
        g_to_depth = false;
        g_dl_opcode_count = 0;
        g_dl_opcode_next = 0;
        g_seq_n = 0; g_seq[0] = 0;
        g_root_n = 0;
        g_combine_freq_n = 0;
        g_prim_color = 0;
        g_env_color = 0;
        g_last_combine_w0 = 0;
        g_last_combine_w1 = 0;
        g_sd_stage = 1;

    };
    if (g_cfg_abtest && g_cfg_fbdump > 0 && static_cast<int>(g_dl_processed) >= g_cfg_fbstart && (g_dl_processed % static_cast<u32>(g_cfg_fbdump)) == 0 && g_fbdump_n + 2U <= 20U) {
        // Comparatif A/B : la meme display list est dessinee deux fois, d'abord en mode strict (copie vers g_ab_xfb), puis en mode ameliore
        const bool saved = g_cfg_enhanced, saved_fog = g_cfg_fog, saved_w2 = g_cfg_water2;
        const bool ab_water = dkr_wii_cfg("abwater", 0.0F) != 0.0F; // cfg abwater=1 : A/B sur la 2e texture de l'eau (A ancien, B nouveau)
        const bool ab_fog = dkr_wii_cfg("abfog", 0.0F) != 0.0F; // cfg abfog=1 : le comparatif A/B porte sur le brouillard (A sans, B avec) au lieu du mode ameliore
        if (g_ab_xfb == nullptr) g_ab_xfb = MEM_K0_TO_K1(memalign(32, static_cast<size_t>(g_wii_rmode->fbWidth) * g_wii_rmode->xfbHeight * VI_DISPLAY_PIX_SZ));
        if (g_ab_xfb != nullptr) {
            if (ab_water) g_cfg_water2 = false; else if (ab_fog) g_cfg_fog = false; else g_cfg_enhanced = false;
            reset_state();
            ParseN64DisplayList(cmd_ptr, cmd_count, rdram_, 0);
            GX_SetDrawDone();
            GX_CopyDisp(static_cast<u8*>(g_ab_xfb) + g_top_pad * g_wii_rmode->fbWidth * VI_DISPLAY_PIX_SZ, GX_TRUE); // efface l'EFB et le Z pour le 2e dessin
            GX_Flush();
            GX_DrawDone();
            g_cfg_enhanced = saved;
            g_cfg_fog = saved_fog;
            g_cfg_water2 = saved_w2;
            g_ab_pending = true;
            g_ab_tag = g_dl_processed;
        }
    }
    if (g_cfg_abmulti && g_abm_req && g_fbdump_n + kAbmVariants + 1U <= 24U) {
        g_abm_req = false;
        ++g_abm_ndone;
        const bool s_sub = g_cfg_subdiv, s_edge = g_edge_subdiv;
        const int s_cull = g_cfg_cull;
        const f32 s_nearw = g_near_w, s_guard = g_guard_band, s_bias = g_decal_bias;
        for (u32 v = 0; v < kAbmVariants; ++v) {
            if (g_abm_xfb[v] == nullptr) g_abm_xfb[v] = MEM_K0_TO_K1(memalign(32, static_cast<size_t>(g_wii_rmode->fbWidth) * g_wii_rmode->xfbHeight * VI_DISPLAY_PIX_SZ));
            if (g_abm_xfb[v] == nullptr) continue;
            switch (v) {
                case 0: g_zw_k = 500.0F; break;
                case 1: g_zw_k = 3000.0F; break;
                case 2: g_blend_zw = true; break;
                case 3: g_cfg_cull = 0; break;
                case 4: g_cfg_subdiv = false; break;
                case 5: g_force_noz = true; break;
                default: g_zw_k = 500.0F; g_blend_zw = true; break;
            }
            reset_state();
            ParseN64DisplayList(cmd_ptr, cmd_count, rdram_, 0);
            GX_SetDrawDone();
            GX_CopyDisp(static_cast<u8*>(g_abm_xfb[v]) + g_top_pad * g_wii_rmode->fbWidth * VI_DISPLAY_PIX_SZ, GX_TRUE);
            GX_Flush();
            GX_DrawDone();
            g_cfg_subdiv = s_sub; g_edge_subdiv = s_edge; g_cfg_cull = s_cull; g_near_w = s_nearw; g_guard_band = s_guard; g_decal_bias = s_bias; g_force_noz = false; g_zw_k = 0.0F; g_blend_zw = false;
        }
        g_abm_pending = true;
        g_abm_tag = g_dl_processed;
    }
    reset_state();
    const u64 prof_t0 = gettime();
    ParseN64DisplayList(cmd_ptr, cmd_count, rdram_, 0);
    const u64 parse_dt = gettime() - prof_t0;
    g_prof_parse += parse_dt;
    g_last_frame_ticks = parse_dt;
    g_sd_stage = 2;
    g_frame_dirty = true;
#endif
}

#if defined(GEKKO)
#include "wii_renderer_profile.inc"
#endif

void dkr::runtime::DiagnosticRenderer::update_screen() {
    const auto index = ++present_count_;
    LoadRenderCfg();
    g_us_stage = 1;
#if defined(GEKKO)
    // Tous les services de debug (mesure d'inactivite, echantillonneur de threads, journal et commandes reseau, chien de garde, releve de profil) ne tournent
    // que si le panneau de debug est actif (lanceur : Debug = Console ou Overlay, cfg diag != 0). Debug sur Arret : le jeu garde toutes les ressources.
    if (g_diag_mode != 0) {
    StartIdleThread();
    StartThreadSampler();
    dkr_netlog_start();
#if defined(DKR_PROFILE_FUNCS)
    dkr_funcprof_start();
#endif
    g_wd_rdram = rdram_;
    StartWatchdog();
    } // g_diag_mode != 0
    g_hb_present = static_cast<unsigned>(index);
    g_hb_dl = g_dl_processed;
    if (rdram_ != nullptr) { s32 mid = 0; std::memcpy(&mid, rdram_ + (dkr::runtime::revision_addresses::CurrentMenuId & 0x007FFFFFU), sizeof(mid)); g_hb_menu = mid; }
#endif
    const u64 prof_upd0 = gettime();
    if (index == 1) {
        // The first screen update is queued only after ultramodern's VI thread
        // has installed and committed its safe dummy VI mode. Starting DKR here
        // prevents the game thread from racing the VI bootstrap state.
        std::fprintf(stderr, "[boot] VI initialized; starting recompiled DKR entrypoint\n");
        recomp::start_game(kGameId);
    }

    if (index <= 10 || index % 60 == 0) {
        std::fprintf(stderr, "[boot][vi] present=%llu dl=%u tri=%u rect=%u\n",
                     static_cast<unsigned long long>(index), g_dl_processed,
                     g_triangles_rendered, g_rects_rendered);
    }
#if defined(GEKKO)
    // update_screen est appelé à 60 Hz mais les display lists arrivent à ~30 Hz : ne copier (et donc effacer
    // l'EFB) que si une nouvelle image a été dessinée, sinon on affiche une image vide une fois sur deux.
    if (g_wii_xfb != nullptr && g_wii_rmode != nullptr && g_frame_dirty) {
        g_frame_dirty = false;
        // (Les carrés de couleur de l'ancien diagnostic ont été retirés : la console texte les remplace.)

        if (g_shot_req != 0) {
            const int color = g_shot_req == 2;
            g_shot_req = 0;
            static u8* zbuf = nullptr;
            const u16 step = color ? 2 : 4;
            const u16 zw = static_cast<u16>(g_view_w / step), zh = static_cast<u16>((g_wii_rmode->efbHeight - g_top_pad) / step);
            if (zbuf == nullptr) zbuf = static_cast<u8*>(std::malloc(static_cast<size_t>(240) * 240 * 3U));
            if (zbuf != nullptr && static_cast<size_t>(zw) * zh <= 240U * 240U) {
                GX_DrawDone();
                u8* o = zbuf;
                for (u16 y = 0; y < zh; ++y) {
                    for (u16 x = 0; x < zw; ++x) {
                        const u16 px = static_cast<u16>(g_view_x + x * step), py = static_cast<u16>(y * step);
                        if (color) {
                            GXColor c;
                            GX_PeekARGB(px, py, &c);
                            *o++ = c.r; *o++ = c.g; *o++ = c.b;
                        } else {
                            u32 z = 0;
                            GX_PeekZ(px, py, &z);
                            *o++ = static_cast<u8>(z >> 16); *o++ = static_cast<u8>(z >> 8); *o++ = static_cast<u8>(z);
                        }
                    }
                }
                if (dkr_shot_submit(zbuf, zw, zh, color) == 0) DbgLog("shot: envoi precedent en cours");
            }
        }
        g_us_stage = 3;
        GX_SetDrawDone();
        GX_CopyDisp(static_cast<u8*>(g_wii_xfb) + g_top_pad * g_wii_rmode->fbWidth * VI_DISPLAY_PIX_SZ, g_pillarbox ? GX_TRUE : GX_FALSE); // bandes : efface l'EFB (noir)
        GX_Flush();
        g_us_stage = 4;

        // Diagnostic texte : la console écrit directement dans le XFB, il faut donc
        // attendre la fin de la copie EFB->XFB puis réécrire le texte à chaque frame.
        { const u64 t0 = gettime(); GX_DrawDone(); g_prof_drawdone += gettime() - t0; }
        g_us_stage = 5;
        if (g_diag_mode == 2) { // overlay : FPS de jeu (display lists) et d'image video, mesures sur ~1 s
            static u64 ov_t = 0;
            static u32 ov_dl = 0;
            static unsigned long long ov_vi = 0;
            static char ov_text[48] = "";
            const u64 now_ov = gettime();
            if (ov_t == 0) { ov_t = now_ov; ov_dl = g_dl_processed; ov_vi = index; }
            const u64 el_us = ticks_to_microsecs(now_ov - ov_t);
            if (el_us >= 1000000ULL) {
                const f32 s = static_cast<f32>(el_us) / 1.0e6F;
                std::snprintf(ov_text, sizeof(ov_text), "%d FPS (%d)", static_cast<int>(static_cast<f32>(g_dl_processed - ov_dl) / s + 0.5F),
                              static_cast<int>(static_cast<f32>(index - ov_vi) / s + 0.5F));
                ov_t = now_ov; ov_dl = g_dl_processed; ov_vi = index;
            }
            if (ov_text[0] != 0) dkr_wii_draw_overlay(g_wii_xfb, static_cast<int>(g_wii_rmode->fbWidth), 12, 10, ov_text);
        }
        if (g_cfg_ring || g_cfg_abmulti) { // L + R + Start (manette GameCube 1) : ecrit ring.txt / debug.txt une fois par appui
            static bool ring_prev = false;
            const u16 held = PAD_ButtonsHeld(0);
            const bool combo = (held & PAD_TRIGGER_L) && (held & PAD_TRIGGER_R) && ((held & PAD_BUTTON_START) || g_cfg_abmulti); // abmulti : L+R suffit (Entree = HOME de la Wiimote emulee = quitter)
            if (combo && !ring_prev) { dkr_dump_flush(); if (g_cfg_abmulti) g_abm_req = true; } // abmulti : L+R+Start declenche aussi le comparatif multi-variantes
            ring_prev = combo;
        }
        bool force_fb = false;
        if (g_ringred_pending) { g_ringred_pending = false; dkr_dump_flush(); }
        if (g_cfg_ringred && !g_ringred_done && index > 4000) { // diagnostic (Dolphin) : declenche l'export quand des pixels rouges satures apparaissent (traces de pneus)
            const u32* px = static_cast<const u32*>(g_wii_xfb) + static_cast<size_t>(g_top_pad) * (g_wii_rmode->fbWidth / 2);
            u32 red = 0;
            const u32 rows = g_wii_rmode->xfbHeight - g_top_pad, half = g_wii_rmode->fbWidth / 2;
            for (u32 y = rows * 55 / 100; y < rows; y += 2) { // bas de l'image seulement (le logo du titre est rouge)
                for (u32 x = 0; x < half; x += 2) {
                    const u32 w = px[static_cast<size_t>(y) * half + x];
                    if (((w >> 24) & 255) < 140 && (w & 255) > 190 && ((w >> 16) & 255) < 120) ++red;
                }
            }
            if (red > static_cast<u32>(g_cfg_ringred) && index >= g_ringred_next) { g_ringred_next = static_cast<u32>(index) + 90; std::snprintf(g_ring_name, sizeof(g_ring_name), "sd:/apps/DKR-R/ring%u.txt", g_ringred_n); ++g_ringred_n; if (g_ringred_n >= 14) g_ringred_done = true; force_fb = true; DbgLog("RINGRED index=%llu red=%u\n", static_cast<unsigned long long>(index), red); g_ringred_pending = true; }
        }
        if (g_cfg_dumpflush > 0 && static_cast<int>(index) >= g_cfg_dumpflush) { g_cfg_dumpflush = 0; dkr_dump_flush(); }
        if (g_abm_pending) { // comparatif multi-variantes : image normale (indice 0) puis les variantes 1..7, ecrites tout de suite sur la SD
            g_abm_pending = false;
            ++g_abm_ndump;
            g_dump_fb_pos = 0; g_fbdump_n = 0; // on ne garde que la derniere capture (le kart attend devant la dalle)
            DumpXfbToBuf(static_cast<const u8*>(g_wii_xfb), g_abm_tag * 16U);
            for (u32 v = 0; v < kAbmVariants; ++v) if (g_abm_xfb[v] != nullptr) DumpXfbToBuf(static_cast<const u8*>(g_abm_xfb[v]), g_abm_tag * 16U + 1U + v);
            dkr_dump_flush();
        }
        if (g_cfg_abmulti) { // capture automatique toutes les ~4 s (compteur de passages : ce bloc ne tourne que sur les images dessinees, pas a chaque vsync)
            static u32 abm_n = 0;
            ++g_abm_npass;
            if (index > 600 && (++abm_n % 120U) == 0U) g_abm_req = true;
        }
        if (g_cfg_abmulti && (index % 20U) == 0U) { // declencheur : la presence du fichier abgo.txt demande une capture
            if (FILE* gf = std::fopen("sd:/apps/DKR-R/abgo.txt", "r")) { std::fclose(gf); std::remove("sd:/apps/DKR-R/abgo.txt"); g_abm_req = true; }
        }
        if (g_ab_pending && g_ab_xfb != nullptr) { // comparatif A/B : image « avant » (mode N64 strict) puis image « apres » (mode ameliore), meme display list
            g_ab_pending = false;
            DumpXfbToBuf(static_cast<const u8*>(g_ab_xfb), g_ab_tag * 2U);
            DumpXfbToBuf(static_cast<const u8*>(g_wii_xfb), g_ab_tag * 2U + 1U);
        } else if (g_cfg_fbdump > 0 && !g_cfg_abtest && static_cast<int>(index) >= g_cfg_fbstart && ((index % static_cast<u32>(g_cfg_fbdump)) == 0 || force_fb)) {
            DumpXfbToBuf(static_cast<const u8*>(g_wii_xfb), static_cast<u32>(index));
        }
        static u32 diag_counter = 0;
        const bool do_diag = g_cfg_diag && (diag_counter++ % 20U) == 0U;
        if (do_diag) {
        // Sauvegarde le curseur (les logs stderr continuent plus bas), écrit le diagnostic en haut
        {
            static bool log_cursor_placed = false;
            if (!log_cursor_placed) {
                log_cursor_placed = true;
                DiagPrint("\x1b[16;0H");
            }
        }
        {
            char tr[16 * 4 + 1];
            int n = 0;
            for (u32 k = 0; k < 16; ++k) {
                const u32 e = g_trace_ring[(g_trace_idx + k) & 15];
                if (e == 0) continue;
                n += std::snprintf(tr + n, sizeof(tr) - n, "%c%u ", static_cast<char>(e >> 8), e & 0xFF);
            }
            tr[n] = 0;
            // 5 opcodes de display list les plus fréquents (opcode:nombre)
            u32 top[5] = {};
            u32 used[5] = {};
            int nt = 0;
            for (int k = 0; k < 5; ++k) {
                u32 best = 0, bi = 0;
                for (u32 o = 0; o < 256; ++o) {
                    bool taken = false;
                    for (int j = 0; j < nt; ++j) if (used[j] == o) taken = true;
                    if (!taken && g_op_hist[o] > best) { best = g_op_hist[o]; bi = o; }
                }
                if (best == 0) break;
                used[nt] = bi; top[nt] = best; ++nt;
            }
            u32 menu_id = 0xFFFFFFFFU;
            if (rdram_ != nullptr) {
                std::memcpy(&menu_id, rdram_ + (dkr::runtime::revision_addresses::CurrentMenuId & 0x007FFFFFU), sizeof(menu_id));
            }
            // Variables de l'écran-titre (US v77, voir extern/dkr-decomp/build/dkr.us.v77.map)
            auto rd32 = [&](u32 addr) -> s32 {
                s32 v = 0;
                if (rdram_ != nullptr) std::memcpy(&v, rdram_ + (addr & 0x007FFFFFU), sizeof(v));
                return v;
            };
            const s32 reveal = rd32(0x8012686C), demo_idx = rd32(0x80126864), demo_timer = rd32(0x80126868);
            const s32 menu_delay = rd32(0x800DF47C);
            auto rd8 = [&](u32 addr) -> int { return rdram_ != nullptr ? static_cast<int>(static_cast<s8>(rdram_[addr & 0x007FFFFFU])) : 0; };
            const int cut_cur = rd8(0x8011AD21), cut_f0 = rd8(0x8011AD22), cut_f1 = rd8(0x8011AD23);
            f32 cine_time = 0.0F;
            if (rdram_ != nullptr) std::memcpy(&cine_time, rdram_ + (0x801268D8U & 0x007FFFFFU), sizeof(cine_time));
            const s32 open_name = rd32(0x801268E0);
            u32 menu_assets = 0;
            for (u32 a = 0; a < 128; ++a) { if (rd32(0x80126550U + a * 4U) != 0) ++menu_assets; }
            char root_hex[72];
            {
                const u32 n = g_root_n < 32 ? g_root_n : 32;
                for (u32 q = 0; q < n; ++q) {
                    std::snprintf(root_hex + q * 2, sizeof(root_hex) - q * 2, "%02X", g_root_ops[(g_root_n - n + q) & 63]);
                }
                root_hex[n * 2] = 0;
            }
            char ob[96];
            int on = std::snprintf(ob, sizeof(ob), "ops");
            for (int k = 0; k < nt; ++k) {
                on += std::snprintf(ob + on, sizeof(ob) - on, " %02X:%u", used[k], top[k]);
            }
            char cc[112];
            int cn = 0;
            u32 cc0 = 0, cc1 = 0;
            for (u32 k = 1; k < g_combine_freq_n; ++k) {
                if (g_combine_freq[k].count > g_combine_freq[cc0].count) cc0 = k;
            }
            if (g_combine_freq_n != 0) {
                cn += std::snprintf(cc + cn, sizeof(cc) - cn, "FC%06X:%08X*%u",
                                    g_combine_freq[cc0].w0 & 0x00FFFFFFU, g_combine_freq[cc0].w1,
                                    g_combine_freq[cc0].count);
                cn += std::snprintf(cc + cn, sizeof(cc) - cn, " P%08X E%08X",
                                    g_combine_freq[cc0].prim, g_combine_freq[cc0].env);
            }
            for (u32 k = 0; k < g_combine_freq_n; ++k) {
                if (k != cc0 && (cc1 == cc0 || g_combine_freq[k].count > g_combine_freq[cc1].count)) cc1 = k;
            }
            if (g_combine_freq_n > 1) {
                cn += std::snprintf(cc + cn, sizeof(cc) - cn, " FC%08X:%08Xx%u",
                                    g_combine_freq[cc1].w0, g_combine_freq[cc1].w1,
                                    g_combine_freq[cc1].count);
            }
            char bind_log[128];
            {
                size_t bl = 0;
                for (u32 q = 0; q < 6 && q < g_bind_n; ++q) bl += std::snprintf(bind_log + bl, sizeof(bind_log) - bl, "%s ", g_bind_ring[(g_bind_n - 1 - q) % 6]);
                bind_log[bl] = 0;
            }
            char dlop[24 * 2 + 1];
            for (u32 k = 0; k < g_dl_opcode_count; ++k) {
                const u32 slot = (g_dl_opcode_next + 24 - g_dl_opcode_count + k) % 24;
                std::snprintf(dlop + k * 2, sizeof(dlop) - k * 2, "%02X", g_dl_opcodes[slot]);
            }
            dlop[g_dl_opcode_count * 2] = 0;
            // Sauvegarde le curseur (les logs stderr continuent plus bas), écrit le diagnostic en haut
            static bool log_cursor_placed = false;
            if (!log_cursor_placed) {
                log_cursor_placed = true;
                DiagPrint("\x1b[7;0H");
            }
            DiagPrint("\x1b[s\x1b[1;0H"
                      "f%llu dl%u tri%u rect%u skip%u E4:%u E5:%u\n"
                      "menu=%d rev=%d di=%d dt=%d ast=%u md=%d\n"
                      "cc %s\n"
                      "tex %s\n"
                      "seq %s\n\x1b[u",
                      static_cast<unsigned long long>(index), g_dl_processed, g_triangles_rendered,
                      g_rects_rendered, g_rects_skipped, g_op_hist[0xE4], g_op_hist[0xE5],
                      static_cast<int>(menu_id), reveal, demo_idx, demo_timer, menu_assets, menu_delay,
                      cc,
                      bind_log,
                      g_seq);
        }
        if (index % 60 == 0) {
            std::fprintf(stderr,
                         "[diag] step=%u thr=%u/%u/%u last_id=%u recv=%u stage=%u loops=%u "
                         "send=%u ext=%u vi_set=%u vi_enq=%u sp=%u gfx=%u\n",
                         g_entrypoint_step, g_threads_created, g_threads_started,
                         g_thread_func_entered, g_thread_last_id_started,
                         g_recv_mesg_calls, g_recv_stage, g_do_recv_loops,
                         g_send_mesg_calls, g_external_messages_dequeued,
                         g_vi_set_event_calls, g_vi_events_enqueued,
                         g_sp_calls, g_sp_gfx_tasks);
            // Même diagnostic dans un fichier de la SD (relu depuis l'hôte, Dolphin synchronise le dossier SD à l'arrêt)
            if (FILE* f = std::fopen("sd:/dkr-diag.txt", "w")) {
                const u8* r = rdram_;
                std::fprintf(f,
                             "frame=%llu step=%u hit=%u\n"
                             "thr created=%u started=%u entered=%u last_id=%u\n"
                             "recv calls=%u stage=%u loops=%u send=%u ext=%u vi_set=%u vi_enq=%u\n"
                             "sp calls=%u gfx=%u aud=%u dl=%u tri=%u rect=%u\n"
                             "thrdbg spawn=%u init=%u woke=%u mism=%u ret=%u term=%u resume=%u\n"
                             "rdram=%p b318=%02X %02X %02X %02X b400=%02X %02X %02X %02X\n",
                             static_cast<unsigned long long>(index), g_entrypoint_step, g_entrypoint_hit,
                             g_threads_created, g_threads_started, g_thread_func_entered, g_thread_last_id_started,
                             g_recv_mesg_calls, g_recv_stage, g_do_recv_loops, g_send_mesg_calls,
                             g_external_messages_dequeued, g_vi_set_event_calls, g_vi_events_enqueued,
                             g_sp_calls, g_sp_gfx_tasks, g_sp_aud_tasks, g_dl_processed,
                             g_triangles_rendered, g_rects_rendered,
                             g_thr_dbg[0], g_thr_dbg[1], g_thr_dbg[2], g_thr_dbg[3], g_thr_dbg[4],
                             g_thr_dbg[5], g_thr_dbg[6],
                             static_cast<const void*>(r),
                             r ? r[0x318] : 0, r ? r[0x319] : 0, r ? r[0x31A] : 0, r ? r[0x31B] : 0,
                             r ? r[0x400] : 0, r ? r[0x401] : 0, r ? r[0x402] : 0, r ? r[0x403] : 0);
                std::fclose(f);
            }
        }
        }
    } else if (g_cfg_diag && g_wii_xfb != nullptr && g_wii_rmode != nullptr && (index % 15) == 0) {
        // Aucune nouvelle display list depuis la dernière copie : signaler l'inactivité (périodes noires)
        DiagPrint("\x1b[s\x1b[7;0HIDLE f%llu dl%u dma%u/%uKB rcv%u snd%u      \n\x1b[u",
                  static_cast<unsigned long long>(index), g_dl_processed, g_pi_dma_calls, g_pi_dma_kb,
                  g_recv_mesg_calls / 2, g_send_mesg_calls / 2);
    }
    g_us_stage = 7;
    { const u64 t0 = gettime(); VIDEO_WaitVSync(); g_prof_vsync += gettime() - t0; }
    g_us_stage = 8;
    g_prof_update += gettime() - prof_upd0;
    if (g_cfg_prof && (index % 120) == 0) WriteProfileReport(index, rdram_);
#endif
}

void dkr::runtime::DiagnosticRenderer::shutdown() {}

std::uint32_t dkr::runtime::DiagnosticRenderer::get_display_framerate() const {
    return 60;
}

float dkr::runtime::DiagnosticRenderer::get_resolution_scale() const {
    return 1.0F;
}

std::unique_ptr<ultramodern::renderer::RendererContext> dkr::runtime::CreateDiagnosticRenderer(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    auto renderer = std::make_unique<DiagnosticRenderer>();
    renderer->rdram_ = rdram;
    return renderer;
}
