// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Profilage : thread inactif, chien de garde, echantillonneur de threads, export XFB, rapport dkr-prof.txt.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

// ---- Mesure du temps CPU inactif : un thread de priorite minimale qui tourne en boucle ; la somme de ses petits intervalles
// (hors preemption) est le temps pendant lequel le processeur n'avait rien d'autre a faire. ----
static volatile u64 g_idle_ticks = 0;
static void* IdleThreadMain(void*) {
    u64 last = gettime();
    for (;;) {
        const u64 now = gettime();
        const u64 d = now - last;
        last = now;
        if (d < 1500) g_idle_ticks += d; // < ~25 us : aucun autre thread n'a pris la main (yield revenu tout de suite)
        LWP_YieldThread();                // cede la place a tout thread pret de priorite egale ; spin seulement si tout le monde attend
    }
    return nullptr;
}
// ---- Chien de garde : si le compteur d'images ne bouge plus pendant 3 s, ecrit sd:/apps/DKR-R/stall.txt (qui attend sur quelle file de
// messages, compteurs de threads) puis recommence toutes les 5 s. ----
extern "C" uint32_t g_blk_mq[64];
extern "C" uint8_t g_blk_op[64];
extern "C" uint32_t g_thr_dbg[8];
static const std::uint8_t* g_wd_rdram = nullptr;
static void* WatchdogThreadMain(void*) {
    unsigned last = 0, still = 0;
    for (;;) {
        usleep(1000000);
        const unsigned now = g_hb_present;
        if (now != last) { last = now; still = 0; continue; }
        if (++still < 3 || (still - 3) % 5 != 0) continue;
        FILE* f = std::fopen("sd:/apps/DKR-R/stall.txt", still == 3 ? "w" : "a");
        if (f == nullptr) continue;
        std::fprintf(f, "STALL %us present=%u dl=%u menu=%d recv_stage=%u loops=%u thr=%u/%u/%u/%u/%u/%u/%u/%u\n", still, g_hb_present, g_hb_dl,
                     static_cast<int>(g_hb_menu), g_recv_stage, g_do_recv_loops, g_thr_dbg[0], g_thr_dbg[1], g_thr_dbg[2], g_thr_dbg[3],
                     g_thr_dbg[4], g_thr_dbg[5], g_thr_dbg[6], g_thr_dbg[7]);
        {   // etat du GPU : FIFO (lecture/ecriture) et drapeaux
            u8 overhi = 0, underlow = 0, readidle = 0, cmdidle = 0, brkpt = 0;
            GX_GetGPStatus(&overhi, &underlow, &readidle, &cmdidle, &brkpt);
            GXFifoObj cpu_fifo, gp_fifo;
            GX_GetCPUFifo(&cpu_fifo);
            GX_GetGPFifo(&gp_fifo);
            void *rd = nullptr, *wr = nullptr, *grd = nullptr, *gwr = nullptr;
            GX_GetFifoPtrs(&cpu_fifo, &rd, &wr);
            GX_GetFifoPtrs(&gp_fifo, &grd, &gwr);
            std::fprintf(f, "  us_stage=%d sd_stage=%d gpu: overhi=%d underlow=%d readidle=%d cmdidle=%d brk=%d cpu_fifo rd=%p wr=%p gp_fifo rd=%p wr=%p cnt=%u\n",
                         static_cast<int>(g_us_stage), static_cast<int>(g_sd_stage), overhi, underlow, readidle, cmdidle, brkpt, rd, wr, grd, gwr,
                         static_cast<unsigned>(GX_GetFifoCount(&gp_fifo)));
        }
        for (unsigned t = 0; t < 64; ++t) {
            if (g_blk_op[t] == 0) continue;
            unsigned valid = 0, count = 0;
            if (g_wd_rdram != nullptr) {
                const u32 a = g_blk_mq[t] & 0x007FFFFFU;
                std::memcpy(&valid, g_wd_rdram + a + 8, 4);
                std::memcpy(&count, g_wd_rdram + a + 16, 4);
            }
            std::fprintf(f, "  thread %u bloque %s sur mq=%08X (valid=%u/%u)\n", t, g_blk_op[t] == 1 ? "recv" : "send", static_cast<unsigned>(g_blk_mq[t]), valid, count);
        }
        std::fclose(f);
    }
    return nullptr;
}
static lwp_t g_wd_thread = 0;
static void StartWatchdog() {
    if (g_wd_thread != 0) return;
    static u8 stack[16384] __attribute__((aligned(32)));
    LWP_CreateThread(&g_wd_thread, WatchdogThreadMain, nullptr, stack, sizeof(stack), 100);
}
// ---- Echantillonneur de threads : une alarme periodique (1 ms, contexte d'interruption) enregistre le thread LWP interrompu ----
static volatile u32 g_samp[32] = {};
static volatile u32 g_samp_id[32] = {};
static char g_lwp_name[32][12] = {};
static int SampSlot(u32 id) {
    for (int i = 0; i < 32; ++i) {
        if (g_samp_id[i] == id) return i;
        if (g_samp_id[i] == 0) { g_samp_id[i] = id; return i; }
    }
    return 31;
}
extern "C" void dkr_name_thread(const char* name) {
    std::strncpy(g_lwp_name[SampSlot(static_cast<u32>(LWP_GetSelf()))], name, 11);
}
extern "C" const char* dkr_thread_name(unsigned slot) { return slot < 32 && g_lwp_name[slot][0] ? g_lwp_name[slot] : "?"; }
#if defined(DKR_PROFILE_FUNCS)
extern "C" void dkr_funcprof_tick(unsigned slot);
#endif
static void SamplerAlarm(syswd_t, void*) {
    const int slot = SampSlot(static_cast<u32>(LWP_GetSelf()));
    ++g_samp[slot];
#if defined(DKR_PROFILE_FUNCS)
    dkr_funcprof_tick(static_cast<unsigned>(slot));
#endif
}
static syswd_t g_samp_alarm;
static void StartThreadSampler() {
    static bool done = false;
    if (done) return;
    done = true;
    SYS_CreateAlarm(&g_samp_alarm);
    struct timespec t0 = {0, 1000000}, period = {0, 1000000};
    SYS_SetPeriodicAlarm(g_samp_alarm, &t0, &period, SamplerAlarm, nullptr);
}
static lwp_t g_idle_thread = 0;
static void StartIdleThread() {
    if (g_idle_thread != 0) return;
    static u8 stack[8192] __attribute__((aligned(32)));
    LWP_CreateThread(&g_idle_thread, IdleThreadMain, nullptr, stack, sizeof(stack), LWP_PRIO_IDLE);
}

// Ajoute une image XFB (YUY2) convertie en RGB au tampon d'export (ecrit par dkr_dump_flush) ; idx = identifiant ecrit dans l'en-tete.
static void DumpXfbToBuf(const u8* xfb, u32 idx) {
    if (g_fbdump_n >= 24U) return;
    const u32 w = g_wii_rmode->fbWidth, h = g_wii_rmode->xfbHeight > static_cast<u32>(g_top_pad) ? g_wii_rmode->xfbHeight - g_top_pad : 0;
    const u32 st = g_cfg_fbfull ? 1U : 2U; // cfg fbfull=1 : images pleine resolution (Dolphin seulement)
    const size_t need = 16 + static_cast<size_t>(w / st) * (h / st) * 3;
    const size_t fb_cap = g_cfg_abmulti ? static_cast<size_t>(2U * 1024U * 1024U) : (g_cfg_fbfull ? kDumpFbCapFull : kDumpFbCap); // abmulti : 8 images demi-resolution ; les 7 tampons XFB des variantes laissent peu de memoire
    if (g_dump_fb == nullptr) g_dump_fb = static_cast<u8*>(std::malloc(fb_cap));
    if (g_dump_fb == nullptr || g_dump_fb_pos + need > fb_cap) return;
    u32 hdr[4] = {0x46423031U, idx, w / st, h / st};
    std::memcpy(g_dump_fb + g_dump_fb_pos, hdr, 16);
    u8* dst = g_dump_fb + g_dump_fb_pos + 16;
    auto clamp = [](int v) { return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    for (u32 y = 0; y < h; y += st) {
        const u8* line = xfb + static_cast<size_t>(g_top_pad + y) * w * 2;
        for (u32 x = 0; x < w; x += st) {
            const u32 xe = x & ~1U; // YUY2 : la chrominance est partagee par la paire de pixels
            const int y0 = line[x * 2], cb = line[xe * 2 + 1], cr = line[xe * 2 + 3];
            const int c = y0 - 16, d = cb - 128, e = cr - 128;
            *dst++ = clamp((298 * c + 409 * e + 128) >> 8);
            *dst++ = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
            *dst++ = clamp((298 * c + 516 * d + 128) >> 8);
        }
    }
    g_dump_fb_pos += need;
    ++g_fbdump_n;
}

// Rapport de performance toutes les 120 images video (~2 s) : sd:/apps/DKR-R/dkr-prof.txt (ajout) + une ligne a l'ecran.
static void WriteProfileReport(unsigned long long index, const std::uint8_t* rdram) {
    static u64 last_time = 0, last_parse = 0, last_tex = 0, last_audio = 0, last_vsync = 0, last_dd = 0, last_upd = 0;
    static u32 last_dl = 0, last_binds = 0, last_miss = 0, last_tri = 0;
    static unsigned long long last_index = 0;
    static bool first = true;
    const u64 now = gettime();
    if (last_time == 0) {
        last_time = now; last_index = index; last_dl = g_dl_processed;
        return;
    }
    const f32 secs = static_cast<f32>(ticks_to_microsecs(now - last_time)) / 1.0e6F;
    if (secs < 0.5F) return;
    if (g_cfg_blenddiag) {
        DbgLog( "--- vi=%llu\n", static_cast<unsigned long long>(index));
        for (u32 k = 0; k < g_blend_freq_n; ++k) {
            DbgLog( "cc=%08X:%08X om_l=%08X cyc=%u prim=%08X env=%08X tris=%u tex=%u va=%u..%u\n", g_blend_freq[k].w0, g_blend_freq[k].w1, g_blend_freq[k].oml,
                         g_blend_freq[k].cyc, g_blend_freq[k].prim, g_blend_freq[k].env, g_blend_freq[k].count, g_blend_freq[k].tex, g_blend_freq[k].amin, g_blend_freq[k].amax);
        }
        
    }
    g_blend_freq_n = 0;
    auto ms = [&](u64 cur, u64 prev) { return static_cast<f32>(ticks_to_microsecs(cur - prev)) / 1000.0F / secs; };
    const f32 present_fps = static_cast<f32>(index - last_index) / secs;
    const f32 dl_fps = static_cast<f32>(g_dl_processed - last_dl) / secs;
    const f32 parse_ms = ms(g_prof_parse, last_parse), tex_ms = ms(g_prof_tex, last_tex), aud_ms = ms(g_prof_audio_ticks, last_audio);
    const f32 vs_ms = ms(g_prof_vsync, last_vsync), dd_ms = ms(g_prof_drawdone, last_dd), upd_ms = ms(g_prof_update, last_upd);
    const u32 dls = g_dl_processed - last_dl;
    const u32 tris = (g_triangles_rendered - last_tri);
    static u64 last_idle = 0;
    const f32 idle_ms = ms(g_idle_ticks, last_idle);
    last_idle = g_idle_ticks;
    const struct mallinfo mi = mallinfo();
    unsigned maxblk_kb = 0;   // plus gros bloc allouable (sonde reelle : mallinfo est peu fiable sur libogc)
    for (unsigned kb : {16384U, 12288U, 8192U, 6144U, 4096U, 3072U, 2048U, 1536U, 1024U, 768U, 512U, 256U, 128U}) {
        void* probe = std::malloc(static_cast<size_t>(kb) * 1024U);
        if (probe != nullptr) { std::free(probe); maxblk_kb = kb; break; }
    }
    u32 menu_id = 0xFFFFFFFFU, obj_count = 0;
    if (rdram != nullptr) {
        std::memcpy(&menu_id, rdram + (dkr::runtime::revision_addresses::CurrentMenuId & 0x007FFFFFU), sizeof(menu_id));
        std::memcpy(&obj_count, rdram + (0x8011AE5CU & 0x007FFFFFU), sizeof(obj_count));
    }
    static u64 last_thr[64] = {};
    static u64 last_ext = 0;
    char thr[96];
    {
        int tn = 0;
        thr[0] = 0;
        // 4 threads les plus actifs (id:ms/s) puis attente externe
        bool used[64] = {};
        for (int rank = 0; rank < 4; ++rank) {
            u64 best = 0; int bi = -1;
            for (int t = 0; t < 64; ++t) if (!used[t] && g_thr_ticks[t] - last_thr[t] > best) { best = g_thr_ticks[t] - last_thr[t]; bi = t; }
            if (bi < 0) break;
            used[bi] = true;
            tn += std::snprintf(thr + tn, sizeof(thr) - tn, "t%d:%.0f ", bi, static_cast<double>(ms(g_thr_ticks[bi], last_thr[bi])));
        }
        std::snprintf(thr + tn, sizeof(thr) - tn, "ext:%.0f", static_cast<double>(ms(g_extwait_ticks, last_ext)));
        for (int t = 0; t < 64; ++t) last_thr[t] = g_thr_ticks[t];
        last_ext = g_extwait_ticks;
    }
    char camtxt[260];
    camtxt[0] = 0;
    if (rdram != nullptr) {   // camera active + premier racer : positions, perche (boomLength), mode
        auto rdw = [&](u32 a) { u32 v; std::memcpy(&v, rdram + (a & 0x007FFFFFU), 4); return v; };
        auto rdf = [&](u32 a) { u32 v = rdw(a); f32 f; std::memcpy(&f, &v, 4); return f; };
        auto rd16 = [&](u32 a) { return static_cast<int>(static_cast<s16>((rdram[(a & 0x7FFFFF)] << 8) | rdram[(a & 0x7FFFFF) + 1])); };
        const u32 camid = rdw(0x80120CE4U) & 7U;
        const u32 cam = 0x80120AC0U + camid * 0x44U;
        const u32 ocount = rdw(0x8011AE5CU), olist = rdw(0x8011AE58U);
        u32 racer = 0;
        for (u32 k = 0; k < ocount && k < 400 && olist != 0 && racer == 0; ++k) {
            const u32 op = rdw(olist + k * 4U);
            if (op != 0 && rd16(op + 0x48U) == 1) racer = op; // BHV_RACER (1 ; 0 = BHV_NONE)
        }
        int n = std::snprintf(camtxt, sizeof(camtxt), "cam%u m%d boom=%.0f p=%.0f,%.0f,%.0f", camid, rd16(cam + 0x36U),
                              static_cast<double>(rdf(cam + 0x1CU)), static_cast<double>(rdf(cam + 0x0CU)),
                              static_cast<double>(rdf(cam + 0x10U)), static_cast<double>(rdf(cam + 0x14U)));
        if (racer != 0) {
            n += std::snprintf(camtxt + n, sizeof(camtxt) - n, " racer=%.0f,%.0f,%.0f s=%.2f",
                               static_cast<double>(rdf(racer + 0x0CU)), static_cast<double>(rdf(racer + 0x10U)),
                               static_cast<double>(rdf(racer + 0x14U)), static_cast<double>(rdf(racer + 0x08U)));
            // sommets du modele courant (curVertData) : 3 premiers (x,y,z s16) + nombre de sommets du modele
            const u32 vtx = rdw(racer + 0x44U);
            const u32 mi = rdw(racer + 0x68U);
            const int midx = static_cast<int>(static_cast<s8>(rdram[(racer + 0x3AU) & 0x7FFFFF]));
            const u32 inst = mi != 0 ? rdw(mi + static_cast<u32>(midx) * 4U) : 0U;
            const u32 mdl = inst != 0 ? rdw(inst) : 0U;
            const int nv = mdl != 0 ? rd16(mdl + 0x24U) : -1;
            if (vtx != 0) std::snprintf(camtxt + n, sizeof(camtxt) - n, " nv=%d v0=%d,%d,%d v1=%d,%d,%d v2=%d,%d,%d", nv,
                                        rd16(vtx), rd16(vtx + 2U), rd16(vtx + 4U), rd16(vtx + 10U), rd16(vtx + 12U), rd16(vtx + 14U),
                                        rd16(vtx + 20U), rd16(vtx + 22U), rd16(vtx + 24U));
        }
    }
    static u32 last_samp[32] = {};
    char samp[120];
    {
        int sn = 0;
        samp[0] = 0;
        u32 total = 0;
        for (int t = 0; t < 32; ++t) total += g_samp[t] - last_samp[t];
        bool used[32] = {};
        for (int rank = 0; rank < 6; ++rank) {
            u32 best = 0; int bi = -1;
            for (int t = 0; t < 32; ++t) if (!used[t] && g_samp[t] - last_samp[t] > best) { best = g_samp[t] - last_samp[t]; bi = t; }
            if (bi < 0 || total == 0) break;
            used[bi] = true;
            sn += std::snprintf(samp + sn, sizeof(samp) - sn, "%s#%d:%u%% ", g_lwp_name[bi][0] ? g_lwp_name[bi] : "?", bi, best * 100U / total);
        }
        for (int t = 0; t < 32; ++t) last_samp[t] = g_samp[t];
    }
    { // diagnostic abmulti : reglage lu, passages dans le bloc de capture, captures effectuees, exports
        const size_t cl = std::strlen(camtxt);
        std::snprintf(camtxt + cl, sizeof(camtxt) - cl, " abm=%d,%u,%u,%u zc=%u,%u", g_cfg_abmulti ? 1 : 0, g_abm_npass, g_abm_ndone, g_abm_ndump, g_zclamp_lo, g_zclamp_hi);
    }
    char line[760];
    std::snprintf(line, sizeof(line),
                  "t=%u vi=%.1f dl=%.1f | ms/s parse=%.0f tex=%.0f(bind %u miss %u) audio=%.0f gxwait=%.0f vsync=%.0f upd=%.0f | tri/dl=%u skip=%d sub=%d | idle=%.0f heap=%u/%uKB maxblk=%uKB menu=%d obj=%u thr=%s smp=%s | %s\n",
                  static_cast<unsigned>(ticks_to_secs(now)), static_cast<double>(present_fps), static_cast<double>(dl_fps),
                  static_cast<double>(parse_ms), static_cast<double>(tex_ms), g_prof_binds - last_binds, g_prof_misses - last_miss,
                  static_cast<double>(aud_ms), static_cast<double>(dd_ms), static_cast<double>(vs_ms), static_cast<double>(upd_ms),
                  dls != 0 ? tris / dls : 0U, g_cfg_skip, g_cfg_subdiv ? 1 : 0,
                  static_cast<double>(idle_ms), static_cast<unsigned>(mi.uordblks >> 10), static_cast<unsigned>(mi.fordblks >> 10), maxblk_kb, static_cast<int>(menu_id), obj_count, thr, samp, camtxt);
    FILE* f = std::fopen("sd:/apps/DKR-R/dkr-prof.txt", first ? "w" : "a");
    first = false;
    if (f != nullptr) { std::fputs(line, f); std::fclose(f); }
    std::memcpy(g_last_prof, line, sizeof(g_last_prof) < sizeof(line) ? sizeof(g_last_prof) : sizeof(line));
    g_last_prof[sizeof(g_last_prof) - 1] = 0;
    dkr_netlog(line);
    last_time = now; last_index = index; last_dl = g_dl_processed; last_parse = g_prof_parse; last_tex = g_prof_tex;
    last_audio = g_prof_audio_ticks; last_vsync = g_prof_vsync; last_dd = g_prof_drawdone; last_upd = g_prof_update;
    last_binds = g_prof_binds; last_miss = g_prof_misses; last_tri = g_triangles_rendered;
    (void)g_prof_tris;
}
