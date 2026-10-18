// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Lecture de dkr-wii.cfg (LoadRenderCfg), reglages modifiables en direct par le canal reseau (dkr_live_*), demande de capture (shot).
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

static void LoadRenderCfg() {
    if (g_cfg_loaded) return;
    g_cfg_loaded = true;
    g_diag_mode = static_cast<int>(dkr_wii_cfg("diag", 0.0F));
    g_cfg_diag = g_diag_mode == 1;
    g_cfg_subdiv = dkr_wii_cfg("subdiv", 1.0F) != 0.0F;
    g_cfg_prof = g_diag_mode != 0 && dkr_wii_cfg("prof", 1.0F) != 0.0F; // releve de profil sur la SD : seulement avec le panneau de debug
    g_cfg_skip = static_cast<int>(dkr_wii_cfg("skip", 0.0F));
    g_cfg_texkb_set = dkr_wii_cfg("texkb", 0.0F) > 0.0F;
    g_tex_cap = static_cast<u32>(dkr_wii_cfg("texkb", 3600.0F)) * 1024U;
    g_cfg_cull = static_cast<int>(dkr_wii_cfg("cull", 1.0F));
    g_cfg_texdump = dkr_wii_cfg("texdump", 0.0F) != 0.0F;
    g_cfg_fbdump = static_cast<int>(dkr_wii_cfg("fbdump", 0.0F));
    g_cfg_water2 = dkr_wii_cfg("water2", 1.0F) != 0.0F;
    g_cfg_water = dkr_wii_cfg("water", 1.0F) != 0.0F;
    g_hud_yscale = dkr_wii_cfg("hudy", 4.0F / 3.0F);
    g_cfg_zupd = dkr_wii_cfg("zupd", 1.0F) != 0.0F;
    g_cfg_abmulti = dkr_wii_cfg("abmulti", 0.0F) != 0.0F;
    g_cfg_shadealpha = dkr_wii_cfg("shadealpha", 1.0F) != 0.0F;
    g_cfg_flatalpha = dkr_wii_cfg("flatalpha", 1.0F) != 0.0F;
    g_cfg_vtxalpha = dkr_wii_cfg("vtxalpha", 0.0F) != 0.0F;
    g_cfg_blenddiag = dkr_wii_cfg("blenddiag", 0.0F) != 0.0F;
    g_cfg_vtxappend_old = dkr_wii_cfg("vtxappend", 0.0F) != 0.0F;
    g_guard_band = dkr_wii_cfg("guard", 0.0F);
    g_edge_subdiv = dkr_wii_cfg("edgesubdiv", 1.0F) != 0.0F;
    g_sky_subdiv = static_cast<int>(dkr_wii_cfg("skysubdiv", 3.0F));
    g_near_w = dkr_wii_cfg("nearw", 0.01F);
    g_cfg_ringred = static_cast<int>(dkr_wii_cfg("ringred", 0.0F));
    g_cfg_skipcc = static_cast<u32>(dkr_wii_cfg("skipcc", 0.0F));
    g_decal_subdiv = static_cast<int>(dkr_wii_cfg("decalsubdiv", 6.0F));
    g_decal_bias = dkr_wii_cfg("decalbias", 0.0003F);
    g_cfg_ring = dkr_wii_cfg("ring", 0.0F) != 0.0F;
    g_cfg_drawinfo = static_cast<int>(dkr_wii_cfg("drawinfo", 0.0F));
    g_cfg_texinfo = dkr_wii_cfg("texinfo", 0.0F) != 0.0F;
    g_cfg_dilate = dkr_wii_cfg("dilate", 1.0F) != 0.0F;
    g_cfg_texoff = dkr_wii_cfg("texoff", 0.0F) != 0.0F;
    g_wrap_margin = dkr_wii_cfg("wrapmargin", 0.03F);
    g_cfg_wrapfix = dkr_wii_cfg("wrapfix", 0.0F) != 0.0F;
    g_cfg_texfilt = dkr_wii_cfg("texfilt", 1.0F) != 0.0F;
    g_cfg_fog = dkr_wii_cfg("fog", 1.0F) != 0.0F;
    g_cfg_abtest = dkr_wii_cfg("abtest", 0.0F) != 0.0F;
    g_cfg_fbfull = dkr_wii_cfg("fbfull", 0.0F) != 0.0F;
    g_cfg_enhanced = dkr_wii_cfg("enhanced_dev", 0.0F) != 0.0F; // mode ameliore desactive (cle renommee : un ancien enhanced=1 ne l'active plus)
    g_cfg_fbstart = static_cast<int>(dkr_wii_cfg("fbstart", 0.0F));
    g_cfg_dumpflush = static_cast<int>(dkr_wii_cfg("dumpflush", 0.0F));
    g_cfg_dynqual = dkr_wii_cfg("dynqual", 0.0F) != 0.0F;
    if (g_cfg_skip < 0) g_cfg_skip = 0;
    if (g_cfg_skip > 3) g_cfg_skip = 3;
}
// Reglages modifiables en direct par le canal de commandes reseau (wii_netlog.cpp, UDP 4301).
static char g_last_prof[760] = {};
extern "C" const char* dkr_live_last_prof() { return g_last_prof; }
static bool g_norect = false;                // live : ne dessine pas les G_FILLRECT
static u32 g_dbg_full_n = 0, g_dbg_full_col = 0, g_dbg_full_cyc = 0, g_dbg_full_zd = 0; // rectangles plein ecran vus
static u32 g_cimg_last = 0, g_cimg_changes = 0, g_tri_cmds = 0, g_tri_depth_skipped = 0, g_zimg_addr = 0xFFFFFFFF; // (deplace ici, utilise par la commande dbg)
extern "C" void dkr_live_dbg(char* out, int cap) {
    std::snprintf(out, static_cast<size_t>(cap), "fullrect n=%u col=%08X cyc=%u zdepth=%u rects=%u tris=%u cimg=%08X zimg=%08X",
                  g_dbg_full_n, g_dbg_full_col, g_dbg_full_cyc, g_dbg_full_zd, g_rects_rendered, g_triangles_rendered, g_cimg_last, g_zimg_addr);
}
struct LiveVar { const char* name; bool* b; f32* f; int* i; };
static LiveVar g_live_vars[] = {
    {"norect", &g_norect, nullptr, nullptr}, {"water2", &g_cfg_water2, nullptr, nullptr}, {"water", &g_cfg_water, nullptr, nullptr}, {"fog", &g_cfg_fog, nullptr, nullptr},
    {"enhanced", &g_cfg_enhanced, nullptr, nullptr}, {"subdiv", &g_cfg_subdiv, nullptr, nullptr},
    {"edgesubdiv", &g_edge_subdiv, nullptr, nullptr}, {"dilate", &g_cfg_dilate, nullptr, nullptr},
    {"texfilt", &g_cfg_texfilt, nullptr, nullptr}, {"vtxalpha", &g_cfg_vtxalpha, nullptr, nullptr},
    {"flatalpha", &g_cfg_flatalpha, nullptr, nullptr}, {"dynqual", &g_cfg_dynqual, nullptr, nullptr},
    {"decalbias", nullptr, &g_decal_bias, nullptr}, {"hudy", nullptr, &g_hud_yscale, nullptr}, {"zupd", &g_cfg_zupd, nullptr, nullptr}, {"nearw", nullptr, &g_near_w, nullptr},
    {"guard", nullptr, &g_guard_band, nullptr}, {"skip", nullptr, nullptr, &g_cfg_skip},
    {"cull", nullptr, nullptr, &g_cfg_cull}, {"skysubdiv", nullptr, nullptr, &g_sky_subdiv},
    {"decalsubdiv", nullptr, nullptr, &g_decal_subdiv},
};
extern "C" int dkr_live_set(const char* key, float v) {
    for (LiveVar& lv : g_live_vars) {
        if (std::strcmp(lv.name, key) != 0) continue;
        if (lv.b) *lv.b = v != 0.0F; else if (lv.f) *lv.f = v; else *lv.i = static_cast<int>(v);
        return 1;
    }
    return 0;
}
extern "C" int dkr_live_get(const char* key, float* out) {
    for (const LiveVar& lv : g_live_vars) {
        if (std::strcmp(lv.name, key) != 0) continue;
        *out = lv.b ? (*lv.b ? 1.0F : 0.0F) : lv.f ? *lv.f : static_cast<float>(*lv.i);
        return 1;
    }
    return 0;
}
extern "C" void dkr_live_list(char* out, int cap) {
    int n = 0;
    out[0] = 0;
    for (const LiveVar& lv : g_live_vars) n += std::snprintf(out + n, static_cast<size_t>(cap - n), "%s ", lv.name);
}
// Capture de la carte de profondeur (commande reseau `shot`) : lecture du Z de l'EFB par GX_PeekZ sur une grille de 4 pixels,
// avant la copie vers l'XFB ; envoi par wii_netlog.cpp (UDP, paquets "ZS").
static volatile int g_shot_req = 0;
extern "C" void dkr_shot_request(int color) { g_shot_req = color ? 2 : 1; }
extern "C" int dkr_shot_submit(const u8* data, u16 w, u16 h, int color); // wii_netlog.cpp : 0 si un envoi precedent est en cours
extern "C" u32 g_pi_dma_kb;
extern "C" u32 g_trace_ring[16];
extern "C" u32 g_trace_idx;

