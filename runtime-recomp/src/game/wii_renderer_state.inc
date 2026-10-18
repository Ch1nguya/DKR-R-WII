// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Etat global du rendu Wii : compteurs, drapeaux de configuration, tampons d'export (fbdump), variables de diagnostic.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

struct DkrVertex {
    s16 x, y, z;
    u8 r, g, b, a;
};

static DkrVertex g_vtx_cache[32];
static Mtx g_mtx_cache[4];
static u32 g_matrix_offset = 0;
static u32 g_vertex_offset = 0;
static u32 g_triangles_rendered = 0;
static u32 g_tri_culled = 0;
static u32 g_rects_rendered = 0;
static u32 g_rects_skipped = 0;
static u32 g_dl_processed = 0;
extern "C" u32 g_entrypoint_hit = 0;
extern "C" u32 g_sp_calls = 0;
extern "C" u32 g_sp_gfx_tasks = 0;
extern "C" u32 g_sp_aud_tasks = 0;
extern "C" u32 g_threads_created;
extern "C" u32 g_threads_started;
extern "C" u32 g_thread_func_entered;
extern "C" u32 g_thread_last_id_started;
extern "C" u32 g_recv_mesg_calls;
extern "C" u32 g_recv_stage;
extern "C" u32 g_send_mesg_calls;
extern "C" u32 g_vi_set_event_calls;
extern "C" u32 g_vi_events_enqueued;
extern "C" u32 g_external_messages_dequeued;
extern "C" u32 g_do_recv_loops;
extern "C" u32 g_entrypoint_step = 0;
extern "C" u32 g_thr_dbg[8];
extern "C" u32 g_pi_dma_calls;
extern "C" float dkr_wii_cfg(const char* key, float def);
extern "C" unsigned long long g_prof_audio_ticks;
unsigned long long g_prof_audio_ticks = 0;
extern "C" uint64_t g_thr_ticks[64];
extern "C" uint64_t g_extwait_ticks;
extern "C" void dkr_netlog(const char* line);
extern "C" void dkr_netlog_start();
#if defined(DKR_PROFILE_FUNCS)
extern "C" void dkr_funcprof_start();
#endif
extern "C" volatile unsigned g_hb_present;
extern "C" volatile unsigned g_hb_dl;
extern "C" volatile int g_hb_menu;
volatile unsigned g_hb_present = 0;
volatile unsigned g_hb_dl = 0;
volatile int g_hb_menu = -1;
static u64 g_prof_parse = 0, g_prof_tex = 0, g_prof_vsync = 0, g_prof_drawdone = 0, g_prof_update = 0;
static u32 g_prof_binds = 0, g_prof_misses = 0, g_prof_tris = 0;
static bool g_cfg_loaded = false, g_cfg_diag = true, g_cfg_subdiv = true, g_cfg_prof = true;
static int g_cfg_skip = 0;
static bool g_cfg_texdump = false;
static int g_cfg_dumpflush = 0;   // cfg dumpflush=N : ecrit les exports sur la SD a l'image video N (utile sous Dolphin, ou HOME est impossible)
static f32 g_guard_band = 0.0F; // cfg guard : decoupe homogene a +-guard * w (0 = desactive)
static bool g_edge_subdiv = true; // cfg edgesubdiv=0 : ancienne subdivision par triangle (coutures de texture entre voisins)
static int g_sky_subdiv = 3; // cfg skysubdiv : niveau maximal pour les triangles dont wmax > 6 wmin
static f32 g_near_w = 0.01F; // cfg nearw : w minimal apres le decoupage near
static f32 g_decal_bias = 0.0003F; // cfg decalbias
static int g_decal_subdiv = 6; // cfg decalsubdiv : niveau maximal de subdivision des triangles en mode decal
static bool g_cfg_dynqual = false; // cfg dynqual=1 (defaut) : adaptation dynamique de la qualite si charge > 33 ms
static u64 g_last_frame_ticks = 0; // duree de la derniere frame graphique (ParseN64DisplayList)
static int g_cycle1_kind = 0;
static bool g_cfg_vtxappend_old = false; // cfg vtxappend=1 : ancien comportement (accumulation)
static int g_cfg_drawinfo = 0; // cfg drawinfo=1 : journal drawinfo.txt (Dolphin uniquement)
static u32 g_stale_n = 0;
static int g_cfg_ringred = 0; static bool g_ringred_done = false; // cfg ringred=N : exporte ring.txt + l'image quand plus de N pixels rouges satures
static bool g_ringred_pending = false;
static u32 g_ringred_next = 0, g_ringred_n = 0;
static u32 g_cfg_skipcc = 0; // cfg skipcc=<hex 16 bits bas de w0 du combiner> : diagnostic, saute ces triangles
static bool g_cfg_ring = false; // cfg ring=1 : anneau des derniers G_VTX / triangles d'objets (L + R + Start ecrit ring.txt)
static u32 g_drawinfo_n = 0, g_drawinfo_flat_n = 0, g_vtxlog_n = 0;
static u32 g_dbg_w0 = 0;
static u32 g_dbg_vtx[6] = {}, g_dbg_vtx_n = 0, g_dbg_vi[3] = {};
static bool g_cfg_texinfo = false; // cfg texinfo=1 : journal texinfo.txt (Dolphin uniquement)
static bool g_cfg_dilate = true; // cfg dilate=0 : garde le RGB d'origine des texels transparents (contours sombres)
static bool g_cfg_texoff = false;  // cfg texoff=0 : sans decalage d'un demi-texel (emulation du filtrage bilineaire du RDP)
static f32 g_wrap_margin = 0.03F; // cfg wrapmargin
static bool g_cfg_wrapfix = false; // cfg wrapfix=0 : toujours le mode wrap/clamp du tile RDP
static bool g_cfg_water2 = true; // cfg water2=0 : ancien rendu (2e texture de l eau avec les coordonnees de la 1re)
static float g_hud_yscale = 4.0F / 3.0F; // cfg hudy : echelle verticale des sprites de HUD en projection orthogonale (defaut 4/3 : pixel pour pixel avec les rectangles de texture ; 1 = ancien rendu)
static bool g_cfg_water = true; // cfg water=0 : ancien rendu de l'eau (texture 0 seule)
static bool g_cfg_shadealpha = true;
static bool g_cfg_flatalpha = true; // cfg flatalpha=0 : surfaces translucides sans texture (traces de pneus, ombres) dessinees opaques (ancien comportement)
static bool g_cfg_vtxalpha = false; // cfg vtxalpha=0 : ignore l'alpha des sommets sur les surfaces translucides (ancien comportement)
static bool g_cfg_blenddiag = false; // cfg blenddiag=1 : ecrit blend.txt (combiners des surfaces translucides) a chaque rapport ; desactive : ecrire pendant le jeu bloque la SD
static bool g_cfg_texfilt = true;  // cfg texfilt=0 : textures en pixels nets (GX_NEAR) ; 1 : bilineaire
static bool g_cfg_fog = true; // cfg fog=0 : sans brouillard (ancien rendu)
static u32 g_fog_rgb = 0;
static f32 g_fog_fm = 0.0F, g_fog_fo = 0.0F;
static bool g_fog_have = false, g_fog_active = false;
static bool g_cfg_abtest = false; // cfg abtest=1 (avec fbdump=N) : chaque N-ieme display list est dessinee deux fois (mode strict puis ameliore) et exportee en paire
// Comparatif multi-variantes (cfg abmulti=1) : sur demande (fichier sd:/apps/DKR-R/abgo.txt), la meme display list est dessinee avec 7 reglages differents,
// puis exportee avec l'image normale (fbdump). Etiquettes : 1 subdiv=0, 2 edgesubdiv=0, 3 cull=0, 4 nearw=1, 5 guard=0.5, 6 sans test de profondeur, 7 sans decal.
constexpr u32 kAbmVariants = 7;
static void* g_abm_xfb[kAbmVariants] = {};
static bool g_cfg_abmulti = false, g_abm_req = false, g_abm_pending = false, g_force_noz = false;
static u32 g_abm_tag = 0;
static u32 g_abm_npass = 0, g_abm_ndone = 0, g_abm_ndump = 0;
static bool g_cfg_zupd = true;                // cfg zupd=0 : ancien comportement (jamais de Z pour les surfaces en melange alpha)
static f32 g_zw_k = 0.0F;                  // profondeur par w (d = w / (w + k)) au lieu de z/w borne ; 0 = ancien calcul (variantes abmulti)
static bool g_blend_zw = false;            // les surfaces en melange alpha ecrivent aussi le Z (variante abmulti)
static u32 g_zclamp_lo = 0, g_zclamp_hi = 0; // sommets dont z/w sortait de [-1, 1] avant d'etre borne (diagnostic, visible dans dkr-prof.txt : zc=bas,haut) // compteurs de diagnostic (visibles dans dkr-prof.txt : abm=cfg,passages,captures,exports)
static void* g_ab_xfb = nullptr;
static bool g_ab_pending = false;
static u32 g_ab_tag = 0;
static bool g_cfg_fbfull = false; // cfg fbfull=1 : fbdump en pleine resolution
static bool g_cfg_enhanced = false; // cfg enhanced=1 : active anticrénelage matériel MSAA et filtrage anisotrope 4x
static int g_cfg_fbstart = 0;     // cfg fbstart=N : ne commence les exports d'images qu'a l'image video N
static int g_cfg_fbdump = 0;      // cfg fbdump=N : ecrit l'image rendue (PPM) toutes les N images dans sd:/apps/DKR-R/fbdump (bureau verrouille, captures impossibles)
static u32 g_fbdump_n = 0;
static constexpr size_t kDumpFbCap = 6U * 1024U * 1024U;
static constexpr size_t kDumpFbCapFull = 11U * 1024U * 1024U; // fbfull=1 (Dolphin seulement ; la memoire invitee ne depasse pas 64 Mo)
static u8* g_dump_fb = nullptr;
static size_t g_dump_fb_pos = 0;
static bool g_cfg_texkb_set = false; // cfg texkb explicite : plafond impose meme en MEM2
static u32 g_tex_cap = 3600U * 1024U; // plafond du cache de textures (octets), cfg texkb
static int g_cfg_cull = 1; // elimination des faces arriere (flag de triangle bit 0x40 absent) : 1 = sens normal, -1 = inverse, 0 = aucune
static volatile int g_us_stage = 0, g_sd_stage = 0; // points de passage de update_screen / send_dl (diagnostic de blocage)
// Ecrit les tampons d'export sur la SD (appelee a la sortie par HOME, quand le jeu ne lit plus la carte).
extern "C" void dkr_dump_flush();
