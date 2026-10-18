// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Etat du RDP emule (matrices, sommets, combiner, othermode...), lecture big-endian de la RDRAM, DiagPrint.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

static inline s16 ReadS16BE(const u8* rdram, u32 addr) {
    s16 val = 0;
    std::memcpy(&val, rdram + (addr & 0x00FFFFFF), sizeof(val));
    return val;
}

static inline u8 ReadU8BE(const u8* rdram, u32 addr) {
    return rdram[addr & 0x00FFFFFF];
}

// Écrit directement sur la console (XFB) sans passer par le verrou/tampon de stdio, qui peut être
// bloqué par un autre thread.
static void DiagPrint(const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        write(STDOUT_FILENO, buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1));
    }
}

struct ClipVertex {
    f32 x, y, z, w;
    u8 r, g, b, a;
};
static f32 g_mvp[3][4][4];
static u8 g_vtx_slot_mvp[32] = {};   // matrice (emplacement 0/1/2) utilisee pour transformer chaque sommet du tampon (diagnostic)
static u32 g_cur_mvp = 0;
static bool g_billboard = false;
static ClipVertex g_clip[32];
static u32 g_vtx_count = 0;
static bool g_to_depth = false;      // le color image courant est le Z-buffer : rien de visible
static u8 g_fill_r = 0, g_fill_g = 0, g_fill_b = 0;
static f32 g_dbg_tri[6] = {};
static u8 g_dbg_tri_col[3] = {};
static u32 g_dbg_rect[5] = {};
static u8 g_root_ops[64] = {};
static u32 g_root_n = 0;
static u32 g_prim_color = 0, g_env_color = 0;
static u32 g_cycle_type = 0;   // RDP : 0 = 1 cycle, 1 = 2 cycles, 2 = copy, 3 = fill (G_SETOTHERMODE_H sft=20 len=2 / G_RDPSETOTHERMODE)
static u32 g_othermode_l = 0;   // mode de rendu RDP (G_SETOTHERMODE_L) ; bit 14 = FORCE_BL (mélange alpha), bit 5 = Z_UPD, bit 4 = Z_CMP
static u32 g_last_combine_w0 = 0, g_last_combine_w1 = 0;
static u32 g_mtx_loads[3] = {}, g_mtx_sels[3] = {}, g_vtx_by_slot[3] = {}, g_tri_by_slot[3] = {};
static u32 g_tr_count = 0;           // G_TEXRECT/FLIP rencontres
static f32 g_tr_last[4] = {};         // ulx uly lrx lry (pixels 320x240) du dernier
static u32 g_tr_last_i[8] = {};       // tile, fmt, siz, w, h, bind_ok, s(raw), t(raw)       // ulx uly lrx lry + couleur 0xRRGGBB (dernier rectangle visible)

