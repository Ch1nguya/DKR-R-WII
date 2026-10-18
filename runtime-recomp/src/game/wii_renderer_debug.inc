// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Export des tampons de diagnostic (dkr_dump_flush), journal de sequence, compteurs de combiners.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

extern "C" void dkr_dump_flush() {
    if (g_ring_n > 0 && g_ring != nullptr) {
        if (FILE* f = std::fopen(g_ring_name, "wb")) {
            const u32 n = g_ring_n < kRingN ? g_ring_n : kRingN;
            for (u32 k = 0; k < n; ++k) std::fputs(g_ring + static_cast<size_t>((g_ring_n - n + k) % kRingN) * kRingW, f);
            std::fclose(f);
        }
    }
    if (g_dbg_pos > 0) {
        if (FILE* f = std::fopen("sd:/apps/DKR-R/debug.txt", "wb")) { std::fwrite(g_dbg_buf, 1, g_dbg_pos, f); std::fclose(f); }
    }
    if (g_dump_tex != nullptr && g_dump_tex_pos > 0) {
        if (FILE* f = std::fopen("sd:/apps/DKR-R/texdump.bin", "wb")) { std::fwrite(g_dump_tex, 1, g_dump_tex_pos, f); std::fclose(f); }
        g_dump_tex_pos = 0;
    }
    if (g_dump_fb != nullptr && g_dump_fb_pos > 0) {
        if (FILE* f = std::fopen("sd:/apps/DKR-R/fbdump.bin", "wb")) { std::fwrite(g_dump_fb, 1, g_dump_fb_pos, f); std::fclose(f); }
    }
}

static char g_seq[40] = {};
static u32 g_seq_n = 0;
static void SeqPush(char c) {
    if (g_seq_n >= 36) { std::memmove(g_seq, g_seq + 1, 35); g_seq_n = 35; }
    g_seq[g_seq_n++] = c;
    g_seq[g_seq_n] = 0;
}
static bool g_frame_dirty = false; // une display list a été dessinée depuis la dernière copie EFB->XFB
static u32 g_dl_budget = 0;
static u32 g_dl_budget_exhausted = 0;
static u32 g_dl_depth_exceeded = 0;
static u32 g_dmadl_count = 0;
static u32 g_dmadl_max_count = 0;
static u32 g_dmadl_rejected = 0;
static u32 g_gdl_rejected = 0;
static u32 g_last_bad_gdl_w1 = 0;
static u32 g_last_bad_dmadl_w1 = 0;
static u32 g_op_hist[256] = {};
static u8 g_dl_opcodes[24] = {};
static u32 g_dl_opcode_count = 0;
static u32 g_dl_opcode_next = 0;
struct CombineFreq { u32 w0, w1, prim, env, count; };
struct BlendFreq { u32 w0, w1, oml, cyc, prim, env, count, tex, amin, amax; };
static BlendFreq g_blend_freq[24] = {};   // diagnostic : surfaces translucides (FORCE_BL) dessinees depuis le dernier rapport
static u32 g_blend_freq_n = 0;
static CombineFreq g_combine_freq[16] = {};
static u32 g_combine_freq_n = 0;

