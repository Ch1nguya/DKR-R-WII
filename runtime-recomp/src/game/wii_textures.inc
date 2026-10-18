// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Inclus uniquement depuis null_renderer.cpp (section GEKKO).
//
// Émulation minimale de la TMEM du RDP et conversion des textures N64 vers des textures GX (RGBA8).
// Flux N64 : G_SETTIMG (source en RDRAM) -> G_LOADBLOCK/G_LOADTILE/G_LOADTLUT (copie vers la TMEM)
//            -> G_SETTILE/G_SETTILESIZE (décrit comment lire la TMEM) -> triangle/rectangle texturé.
// Ici : la TMEM est un tableau de 4 Ko ; la conversion se fait à la demande au moment du dessin (TexBind),
// avec un cache indexé par un hash du contenu utile de la TMEM.

struct TileDesc {
    u8 fmt = 0, siz = 0, pal = 0, cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
    u16 line = 0, tmem = 0;
    u16 uls = 0, ult = 0, lrs = 0, lrt = 0; // 10.2
    bool has_size = false;
};
static TileDesc g_tiles[8];
alignas(8) static u8 g_tmem[4096];
static u16 g_pal[256];
static struct {
    u8 fmt, siz;
    u16 width;
    u32 addr;
} g_timg = {0, 0, 1, 0};
static bool g_tex_dirty = true;
static u8 g_tev_n = 1; // nombre d'etages TEV courant (suivi pour ajouter l'etage de brouillard)
static inline void TevStages(u8 n) { g_tev_n = n; GX_SetNumTevStages(n); }
static bool g_textured_mode = false;

struct GxTex {
    void* data;
    GXTexObj obj;
    u16 w, h;
    u32 bytes;
    u32 id;
};
// Cache de textures en MEM2 (64 Mo de memoire rapide supplementaire) : allocateur lineaire remis a zero a chaque vidage du cache.
// Repli sur memalign (MEM1) si la MEM2 n'est pas disponible.
static u8* g_m2_base = nullptr;
static size_t g_m2_size = 0, g_m2_pos = 0;
static bool g_m2_tried = false;
static void M2Init() {
    if (g_m2_tried) return;
    g_m2_tried = true;
    const u32 lo = reinterpret_cast<u32>(SYS_GetArena2Lo()), hi = reinterpret_cast<u32>(SYS_GetArena2Hi());
    if (hi <= lo) return;
    const u32 avail = hi - lo;
    if (avail < 16U * 1024U * 1024U) return;
    size_t want = static_cast<size_t>(avail) - 8U * 1024U * 1024U; // laisse de la marge aux autres modules (reseau, son)
    if (want > 40U * 1024U * 1024U) want = 40U * 1024U * 1024U;
    const u32 base = (lo + 31U) & ~31U; // l'arene MEM2 est reduite par le bas : le reste de la MEM2 reste disponible aux autres modules
    SYS_SetArena2Lo(reinterpret_cast<void*>(base + static_cast<u32>(want)));
    g_m2_base = reinterpret_cast<u8*>(base);
    g_m2_size = want;
}
static inline bool InM2(const void* p) {
    const u8* q = static_cast<const u8*>(p);
    return g_m2_base != nullptr && q >= g_m2_base && q < g_m2_base + g_m2_size;
}
static void* TexAlloc(size_t size) {
    M2Init();
    const size_t sz = (size + 31U) & ~static_cast<size_t>(31U);
    if (g_m2_base != nullptr && g_m2_pos + sz <= g_m2_size) {
        void* p = g_m2_base + g_m2_pos;
        g_m2_pos += sz;
        return p;
    }
    return memalign(32, size);
}
static u32 g_tex_bytes = 0;     // memoire totale des textures en cache
static std::unordered_map<u64, GxTex*> g_tex_cache;
static GxTex* g_cur_tex = nullptr;
static u8 g_bind_map = GX_TEXMAP0; // unite de texture GX ciblee par TexBind (TEXMAP1 : seconde texture, TEXEL1 du combiner)
static u16 g_cur_w = 32, g_cur_h = 32; // dimensions (multiples de 4) de la texture courante
static u32 g_tex_created = 0;
static char g_bind_ring[6][20];
static u32 g_bind_n = 0;

static inline u32 TexBits(u8 siz) { return 4U << siz; }

static void TexSetTimg(u32 w0, u32 w1) {
    g_timg.fmt = (w0 >> 21) & 7;
    g_timg.siz = (w0 >> 19) & 3;
    g_timg.width = static_cast<u16>((w0 & 0xFFF) + 1);
    g_timg.addr = w1 & 0x00FFFFFF;
}

static void TexSetTile(u32 w0, u32 w1) {
    TileDesc& t = g_tiles[(w1 >> 24) & 7];
    t.fmt = (w0 >> 21) & 7;
    t.siz = (w0 >> 19) & 3;
    t.line = (w0 >> 9) & 0x1FF;
    t.tmem = w0 & 0x1FF;
    t.pal = (w1 >> 20) & 0xF;
    t.cmt = (w1 >> 18) & 3;
    t.maskt = (w1 >> 14) & 0xF;
    t.shiftt = (w1 >> 10) & 0xF;
    t.shifts = w1 & 0xF;
    t.cms = (w1 >> 8) & 3;
    t.masks = (w1 >> 4) & 0xF;
    g_tex_dirty = true;
}

static void TexSetTileSize(u32 w0, u32 w1) {
    TileDesc& t = g_tiles[(w1 >> 24) & 7];
    t.uls = (w0 >> 12) & 0xFFF;
    t.ult = w0 & 0xFFF;
    t.lrs = (w1 >> 12) & 0xFFF;
    t.lrt = w1 & 0xFFF;
    t.has_size = true;
    g_tex_dirty = true;
}

static u32 g_last_load[6] = {};   // diagnostic : type(1=block,2=tile), dxt/lrs, tmem, fmt|siz de l'image source, largeur source, ult
static u32 g_texdump_n = 0;
static constexpr size_t kDumpTexCap = 2560U * 1024U;
static u8* g_dump_tex = nullptr;
static size_t g_dump_tex_pos = 0;
static void TexLoadBlock(const u8* rdram, u32 w0, u32 w1) {
    const TileDesc& t = g_tiles[(w1 >> 24) & 7];
    const u32 uls = (w0 >> 12) & 0xFFF;
    const u32 ult = w0 & 0xFFF;
    const u32 texels = ((w1 >> 12) & 0xFFF) + 1;
    const u32 bits = TexBits(g_timg.siz);
    const u32 bytes = (texels * bits) / 8;
    const u32 src = g_timg.addr + ((ult * g_timg.width + uls) * bits) / 8;
    const u32 dst = static_cast<u32>(t.tmem) * 8;
    const u32 dxt = w1 & 0xFFF;
    g_last_load[0] = 1; g_last_load[1] = dxt; g_last_load[2] = t.tmem; g_last_load[3] = (g_timg.fmt << 4) | g_timg.siz; g_last_load[4] = g_timg.width; g_last_load[5] = ult;
    const bool rgba32 = t.fmt == 0 && t.siz == 3;
    for (u32 i = 0; i < bytes && i < 4096; ++i) {
        // Le RDP échange les deux moitiés de 32 bits des mots de 64 bits sur les lignes impaires (comptées via dxt)
        const u32 line = ((i >> 3) * dxt) >> 11;
        // RGBA32 : le RDP range 4 texels par mot (RG dans une banque, BA dans l'autre) ; echanger les moities de 32 bits d'un mot
        // revient a echanger les paires de texels (x^2) = 8 octets de la copie lineaire (et non 4 : cela decalait d'un texel les lignes impaires)
        const bool rgba32_load = g_timg.siz == 3;
        const u32 idx = (line & 1) ? (rgba32_load ? (i ^ 8U) : (i ^ 4U)) : i;
        g_tmem[(dst + idx) & 0xFFF] = rdram[(src + i) & 0x007FFFFF];
    }
    g_tex_dirty = true;
}

static void TexLoadTile(const u8* rdram, u32 w0, u32 w1) {
    const TileDesc& t = g_tiles[(w1 >> 24) & 7];
    const u32 uls = ((w0 >> 12) & 0xFFF) >> 2;
    const u32 ult = (w0 & 0xFFF) >> 2;
    const u32 lrs = ((w1 >> 12) & 0xFFF) >> 2;
    const u32 lrt = (w1 & 0xFFF) >> 2;
    const u32 bits = TexBits(g_timg.siz);
    const u32 src_pitch = (g_timg.width * bits) / 8;
    const u32 row_bytes = ((lrs - uls + 1) * bits + 7) / 8;
    const u32 dst_pitch = static_cast<u32>(t.line) * 8;
    g_last_load[0] = 2; g_last_load[1] = lrs; g_last_load[2] = t.tmem; g_last_load[3] = (g_timg.fmt << 4) | g_timg.siz; g_last_load[4] = g_timg.width; g_last_load[5] = ult;
    for (u32 r = ult; r <= lrt && (r - ult) < 256; ++r) {
        const u32 src = g_timg.addr + r * src_pitch + (uls * bits) / 8;
        const u32 dst = static_cast<u32>(t.tmem) * 8 + (r - ult) * dst_pitch;
        const bool odd = ((r - ult) & 1) != 0;
        const u32 swap_bit = g_timg.siz == 3 ? 8U : 4U; // RGBA32 : paires de texels (voir TexLoadBlock)
        for (u32 i = 0; i < row_bytes; ++i) {
            g_tmem[(dst + (odd ? (i ^ swap_bit) : i)) & 0xFFF] = rdram[(src + i) & 0x007FFFFF];
        }
    }
    g_tex_dirty = true;
}

static void TexLoadTlut(const u8* rdram, u32 w0, u32 w1) {
    (void)w0;
    const TileDesc& t = g_tiles[(w1 >> 24) & 7];
    const u32 count = ((w1 >> 14) & 0x3FF) + 1;
    for (u32 i = 0; i < count; ++i) {
        const int idx = static_cast<int>(t.tmem) - 256 + static_cast<int>(i);
        if (idx < 0 || idx >= 256) continue;
        const u32 a = (g_timg.addr + i * 2) & 0x007FFFFF;
        g_pal[idx] = static_cast<u16>((rdram[a] << 8) | rdram[a + 1]);
    }
    g_tex_dirty = true;
}

static inline u32 PackRGBA(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }

static inline u32 DecodeRGBA16(u16 c) {
    const u32 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
    return PackRGBA((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (c & 1) ? 0xFF : 0x00);
}

// Lit un texel (x, y) de la tuile depuis la TMEM et le renvoie en 0xRRGGBBAA.
static u32 FetchTexel(const TileDesc& t, u32 x, u32 y, u32 pitch) {
    const u32 row = static_cast<u32>(t.tmem) * 8 + y * pitch;
    const u32 swap = (y & 1) ? ((t.fmt == 0 && t.siz == 3) ? 8U : 4U) : 0U; // lecture : échange sur les lignes impaires (RGBA32 : paires de texels)
    auto byte_at = [&](u32 off) -> u32 { return g_tmem[(row + (off ^ swap)) & 0xFFF]; };
    switch (t.fmt) {
    case 0: // RGBA
        if (t.siz == 2) {
            const u32 o = x * 2;
            return DecodeRGBA16(static_cast<u16>((byte_at(o) << 8) | byte_at(o + 1)));
        }
        if (t.siz == 3) {
            const u32 o = x * 4;
            return PackRGBA(byte_at(o), byte_at(o + 1), byte_at(o + 2), byte_at(o + 3));
        }
        break;
    case 2: { // CI : indices dans la palette (RGBA16)
        u32 idx;
        if (t.siz == 0) {
            const u32 b = byte_at(x >> 1);
            idx = ((x & 1) ? (b & 0xF) : (b >> 4)) + static_cast<u32>(t.pal) * 16;
        } else {
            idx = byte_at(x);
        }
        return DecodeRGBA16(g_pal[idx & 0xFF]);
    }
    case 3: // IA
        if (t.siz == 2) {
            const u32 i = byte_at(x * 2), a = byte_at(x * 2 + 1);
            return PackRGBA(i, i, i, a);
        } else if (t.siz == 1) {
            const u32 b = byte_at(x);
            const u32 i = (b >> 4) * 17, a = (b & 0xF) * 17;
            return PackRGBA(i, i, i, a);
        } else if (t.siz == 0) {
            const u32 b = byte_at(x >> 1);
            const u32 n = (x & 1) ? (b & 0xF) : (b >> 4);
            const u32 i = ((n >> 1) * 255) / 7, a = (n & 1) ? 0xFF : 0;
            return PackRGBA(i, i, i, a);
        }
        break;
    case 4: // I (sur le RDP, l'alpha vaut l'intensité)
        if (t.siz == 1) {
            const u32 i = byte_at(x);
            return PackRGBA(i, i, i, i);
        } else if (t.siz == 0) {
            const u32 b = byte_at(x >> 1);
            const u32 n = ((x & 1) ? (b & 0xF) : (b >> 4)) * 17;
            return PackRGBA(n, n, n, n);
        }
        break;
    default:
        break;
    }
    return PackRGBA(0xFF, 0x00, 0xFF, 0xFF); // format non géré : magenta bien visible
}

// Prépare (décodage + cache) la texture décrite par la tuile et la charge dans GX_TEXMAP0.
static bool TexBindImpl(u32 tile_index) {
    const TileDesc& t = g_tiles[tile_index & 7];
    const u32 w = t.masks ? (1U << t.masks) : (t.has_size ? (((t.lrs - t.uls) >> 2) + 1U) : 32U);
    const u32 h = t.maskt ? (1U << t.maskt) : (t.has_size ? (((t.lrt - t.ult) >> 2) + 1U) : 32U);
    if (w == 0 || h == 0 || w > 256 || h > 256) return false;
    const u32 bits = TexBits(t.siz);
    u32 pitch = static_cast<u32>(t.line) * 8;
    // En RGBA32, `line` compte les mots de 64 bits d'une moitie de la TMEM (RG ou BA) : dans notre copie lineaire
    // (R,G,B,A contigus) une ligne occupe deux fois plus d'octets.
    if (t.fmt == 0 && t.siz == 3) pitch *= 2;
    if (pitch == 0) pitch = (w * bits + 7) / 8;
    const u32 w4 = (w + 3) & ~3U, h4 = (h + 3) & ~3U;
    std::snprintf(g_bind_ring[g_bind_n++ % 6], sizeof(g_bind_ring[0]), "%u%u:%ux%uL%u", t.fmt, t.siz, w, h, t.line);

    // Hash du contenu utile (paramètres de la tuile, octets de la TMEM lus, palette)
    u64 key = 1469598103934665603ULL;
    auto mix = [&](u32 v) {
        key ^= v;
        key *= 1099511628211ULL;
    };
    mix(g_cfg_enhanced ? 1U : 0U); mix(t.fmt); mix(t.siz); mix(w); mix(h); mix(pitch); mix(t.cms); mix(t.cmt); mix(t.pal);
    const u32 base = static_cast<u32>(t.tmem) * 8;
    const u32 row_bytes = (w * bits + 7) / 8;
    {   // hash du contenu par mots de 32 bits (4x moins d'iterations que par octet) ; 2 etats independants
        u32 h1 = 0x811C9DC5U, h2 = 0x01000193U;
        for (u32 r = 0; r < h; ++r) {
            const u32 row = base + r * pitch;
            u32 b = 0;
            for (; b + 4 <= row_bytes; b += 4) {
                const u32 wd = *reinterpret_cast<const u32*>(&g_tmem[(row + b) & 0xFFC]);
                h1 = ((h1 << 5) | (h1 >> 27)) ^ wd;
                h1 *= 0x9E3779B1U;
                h2 = h2 * 31U + wd;
            }
            for (; b < row_bytes; ++b) h1 = (h1 ^ g_tmem[(row + b) & 0xFFF]) * 16777619U;
        }
        mix(h1);
        mix(h2);
    }
    if (t.fmt == 2) {
        const u32 first = t.siz == 0 ? static_cast<u32>(t.pal) * 16 : 0;
        const u32 n = t.siz == 0 ? 16 : 256;
        for (u32 i = 0; i < n; ++i) mix(g_pal[(first + i) & 0xFF]);
    }

    GxTex* tex = nullptr;
    auto it = g_tex_cache.find(key);
    if (it != g_tex_cache.end()) {
        tex = it->second;
    } else {
        auto flush_cache = [&]() { // attendre la fin du dessin puis tout liberer
            GX_DrawDone();
            for (auto& kv : g_tex_cache) {
                if (!InM2(kv.second->data)) free(kv.second->data);
                delete kv.second;
            }
            g_tex_cache.clear();
            g_tex_bytes = 0;
            g_m2_pos = 0;
            g_cur_tex = nullptr;
        };
        // Format GX : RGBA16/CI -> RGB5A3 (2 octets), I/IA -> IA8 (2 octets), RGBA32 -> RGBA8 (4 octets)
        const bool rgba32 = (t.fmt == 0 && t.siz == 3);
        const bool ia_mode = (t.fmt == 3 || t.fmt == 4);
        const u8 gx_fmt = rgba32 ? GX_TF_RGBA8 : (ia_mode ? GX_TF_IA8 : GX_TF_RGB5A3);
        const bool pow2 = ((w4 & (w4 - 1)) == 0) && ((h4 & (h4 - 1)) == 0);
        const bool can_mipmap = g_cfg_enhanced && pow2 && (w4 >= 8 || h4 >= 8);

        u32 num_lods = 1;
        if (can_mipmap) {
            u32 dim = w4 > h4 ? w4 : h4;
            while (dim > 1) {
                dim >>= 1;
                ++num_lods;
            }
        }

        auto calc_level_size = [&](u32 lw, u32 lh) -> size_t {
            const u32 l_w4 = (lw + 3) & ~3U;
            const u32 l_h4 = (lh + 3) & ~3U;
            return static_cast<size_t>(l_w4) * l_h4 * (rgba32 ? 4 : 2);
        };

        size_t size = 0;
        {
            u32 cur_w = w4, cur_h = h4;
            for (u32 lvl = 0; lvl < num_lods; ++lvl) {
                size += calc_level_size(cur_w, cur_h);
                cur_w = (cur_w > 1) ? (cur_w >> 1) : 1;
                cur_h = (cur_h > 1) ? (cur_h >> 1) : 1;
            }
        }

        M2Init();
        const size_t cap_now = (g_m2_base != nullptr && !g_cfg_texkb_set) ? g_m2_size : g_tex_cap; // MEM2 : plafond = taille reservee, sauf cfg texkb explicite
        if (g_tex_cache.size() > (g_m2_base != nullptr ? 6000U : 700U) || g_tex_bytes + size > cap_now) flush_cache();
        tex = new GxTex();
        tex->w = static_cast<u16>(w4);
        tex->h = static_cast<u16>(h4);
        tex->bytes = static_cast<u32>(size);
        tex->data = TexAlloc(size);
        if (tex->data == nullptr) { // memoire epuisee : liberer le cache et reessayer une fois
            flush_cache();
            tex->data = TexAlloc(size);
            if (tex->data == nullptr) { delete tex; return false; }
        }
        g_tex_bytes += tex->bytes;
        u8* out = static_cast<u8*>(tex->data);
        // Decodage complet puis « bleed » des couleurs : le RGB des texels totalement transparents (souvent noir) est remplace par la moyenne des voisins
        // visibles. Le filtre bilineaire de GX melange sinon ce noir avec les bords des sprites (contours sombres sur la vegetation).
        std::vector<u32> dec(static_cast<size_t>(w) * h);
        bool has_clear = false, has_solid = false;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u32 px = FetchTexel(t, x, y, pitch);
                dec[static_cast<size_t>(y) * w + x] = px;
                if ((px & 0xFF) == 0) has_clear = true; else if ((px & 0xFF) >= 0x80) has_solid = true;
            }
        }
        if (g_cfg_dilate && has_clear && has_solid) {
            std::vector<u8> valid(dec.size());
            for (size_t k = 0; k < dec.size(); ++k) valid[k] = (dec[k] & 0xFF) >= 0x80 ? 1 : 0;
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<u32> nxt = dec;
                std::vector<u8> nvalid = valid;
                bool changed = false;
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        const size_t k = static_cast<size_t>(y) * w + x;
                        if (valid[k] || (dec[k] & 0xFF) != 0) continue; // seuls les texels totalement transparents sont recolores
                        u32 sr = 0, sg = 0, sb = 0, n = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
                                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= static_cast<int>(w) || ny >= static_cast<int>(h)) continue;
                                const size_t kk = static_cast<size_t>(ny) * w + nx;
                                if (!valid[kk]) continue;
                                sr += (dec[kk] >> 24) & 0xFF; sg += (dec[kk] >> 16) & 0xFF; sb += (dec[kk] >> 8) & 0xFF; ++n;
                            }
                        }
                        if (n != 0) { nxt[k] = ((sr / n) << 24) | ((sg / n) << 16) | ((sb / n) << 8); nvalid[k] = 1; changed = true; }
                    }
                }
                dec.swap(nxt);
                valid.swap(nvalid);
                if (!changed) break;
            }
        }

        auto pack_level = [&](u8* dst_blk, const std::vector<u32>& src_px, u32 cur_w, u32 cur_h) {
            const u32 cur_w4 = (cur_w + 3) & ~3U;
            const u32 cur_h4 = (cur_h + 3) & ~3U;
            for (u32 y = 0; y < cur_h4; ++y) {
                for (u32 x = 0; x < cur_w4; ++x) {
                    const u32 px = src_px[static_cast<size_t>(y < cur_h ? y : cur_h - 1) * cur_w + (x < cur_w ? x : cur_w - 1)];
                    const u32 r = (px >> 24) & 0xFF, g = (px >> 16) & 0xFF, b = (px >> 8) & 0xFF, a = px & 0xFF;
                    if (rgba32) {
                        u8* blk = dst_blk + ((y >> 2) * (cur_w4 >> 2) + (x >> 2)) * 64;
                        const u32 pp = (y & 3) * 4 + (x & 3);
                        blk[pp * 2] = static_cast<u8>(a);
                        blk[pp * 2 + 1] = static_cast<u8>(r);
                        blk[32 + pp * 2] = static_cast<u8>(g);
                        blk[32 + pp * 2 + 1] = static_cast<u8>(b);
                    } else {
                        u8* blk = dst_blk + ((y >> 2) * (cur_w4 >> 2) + (x >> 2)) * 32;
                        const u32 pp = (y & 3) * 4 + (x & 3);
                        u16 v;
                        if (ia_mode) {
                            v = static_cast<u16>((a << 8) | r);
                        } else if (a >= 0xE0) {
                            v = static_cast<u16>(0x8000U | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                        } else {
                            v = static_cast<u16>(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                        }
                        blk[pp * 2] = static_cast<u8>(v >> 8);
                        blk[pp * 2 + 1] = static_cast<u8>(v & 0xFF);
                    }
                }
            }
        };

        // Encodage du niveau 0
        pack_level(out, dec, w, h);

        // Encodage des niveaux de mipmap suivants (1 .. num_lods - 1)
        if (num_lods > 1) {
            u8* cur_dst = out + calc_level_size(w4, h4);
            std::vector<u32> prev_img = dec;
            u32 prev_w = w, prev_h = h;

            for (u32 lvl = 1; lvl < num_lods; ++lvl) {
                const u32 next_w = (prev_w > 1) ? (prev_w >> 1) : 1;
                const u32 next_h = (prev_h > 1) ? (prev_h >> 1) : 1;
                std::vector<u32> next_img(static_cast<size_t>(next_w) * next_h);

                for (u32 ny = 0; ny < next_h; ++ny) {
                    for (u32 nx = 0; nx < next_w; ++nx) {
                        const u32 x0 = nx * 2, x1 = (x0 + 1 < prev_w) ? (x0 + 1) : x0;
                        const u32 y0 = ny * 2, y1 = (y0 + 1 < prev_h) ? (y0 + 1) : y0;

                        const u32 p00 = prev_img[y0 * prev_w + x0];
                        const u32 p01 = prev_img[y0 * prev_w + x1];
                        const u32 p10 = prev_img[y1 * prev_w + x0];
                        const u32 p11 = prev_img[y1 * prev_w + x1];

                        const u32 r = (((p00 >> 24) & 0xFF) + ((p01 >> 24) & 0xFF) + ((p10 >> 24) & 0xFF) + ((p11 >> 24) & 0xFF) + 2) >> 2;
                        const u32 g = (((p00 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;
                        const u32 b = (((p00 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF) + ((p10 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF) + 2) >> 2;
                        const u32 a = ((p00 & 0xFF) + (p01 & 0xFF) + (p10 & 0xFF) + (p11 & 0xFF) + 2) >> 2;

                        next_img[ny * next_w + nx] = (r << 24) | (g << 16) | (b << 8) | a;
                    }
                }

                pack_level(cur_dst, next_img, next_w, next_h);
                cur_dst += calc_level_size(next_w, next_h);
                prev_img = std::move(next_img);
                prev_w = next_w;
                prev_h = next_h;
            }
        }

        DCFlushRange(tex->data, size);
        const u8 wrap_s = (t.cms & 2) ? GX_CLAMP : ((t.cms & 1) ? GX_MIRROR : GX_REPEAT);
        const u8 wrap_t = (t.cmt & 2) ? GX_CLAMP : ((t.cmt & 1) ? GX_MIRROR : GX_REPEAT);
        const u8 mipmap_flag = (num_lods > 1) ? GX_TRUE : GX_FALSE;
        GX_InitTexObj(&tex->obj, tex->data, w4, h4, gx_fmt, pow2 ? wrap_s : GX_CLAMP,
                      pow2 ? wrap_t : GX_CLAMP, mipmap_flag);

        if (num_lods > 1) {
            GX_InitTexObjLOD(&tex->obj, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, static_cast<float>(num_lods - 1),
                             0.0f, GX_ENABLE, GX_ENABLE, GX_ANISO_4);
        } else {
            GX_InitTexObjFilterMode(&tex->obj, g_cfg_texfilt ? GX_LINEAR : GX_NEAR, g_cfg_texfilt ? GX_LINEAR : GX_NEAR);
            if (g_cfg_enhanced) {
                GX_InitTexObjEdgeLOD(&tex->obj, GX_ENABLE);
                GX_InitTexObjMaxAniso(&tex->obj, GX_ANISO_4);
            }
        }
        g_tex_cache[key] = tex;
        ++g_tex_created;
        tex->id = g_tex_created;
        if (g_cfg_texinfo) { // diagnostic (Dolphin uniquement) : parametres du tile de chaque texture creee
            {
                DbgLog( "n=%u %ux%u(h4=%u,w4=%u) fmt=%u siz=%u masks=%u maskt=%u cms=%u cmt=%u uls=%u ult=%u lrs=%u lrt=%u sized=%d line=%u tmem=%u ld=%u/%u/%u/%u/%u/%u\n",
                             g_tex_created, w, h, h4, w4, t.fmt, t.siz, t.masks, t.maskt, t.cms, t.cmt, t.uls, t.ult, t.lrs, t.lrt, t.has_size ? 1 : 0, t.line, t.tmem,
                             g_last_load[0], g_last_load[1], g_last_load[2], g_last_load[3], g_last_load[4], g_last_load[5]);
                
            }
        }
        if (g_cfg_texdump && g_texdump_n < 300) {
            // Tampon en RAM (ecrit sur la SD seulement a la sortie par HOME : ecrire pendant le jeu a bloque le thread graphique / la SD)
            const size_t need = 72 + static_cast<size_t>(w) * h * 4;
            if (g_dump_tex == nullptr) g_dump_tex = static_cast<u8*>(std::malloc(kDumpTexCap));
            if (g_dump_tex != nullptr && g_dump_tex_pos + need <= kDumpTexCap) {
                u32 hdr[18] = {0x54455831U, g_texdump_n, w, h, t.fmt, t.siz, t.line, t.tmem, t.pal, t.cms, t.cmt,
                               g_last_load[0], g_last_load[1], g_last_load[2], g_last_load[3], g_last_load[4], g_last_load[5], 0};
                std::memcpy(g_dump_tex + g_dump_tex_pos, hdr, 72);
                u8* dst = g_dump_tex + g_dump_tex_pos + 72;
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        const u32 px = FetchTexel(t, x, y, pitch);
                        *dst++ = static_cast<u8>(px >> 24); *dst++ = static_cast<u8>(px >> 16); *dst++ = static_cast<u8>(px >> 8); *dst++ = static_cast<u8>(px);
                    }
                }
                g_dump_tex_pos += need;
            }
            ++g_texdump_n;
        }
        GX_InvalidateTexAll(); // memoire de texture nouvelle (peut recycler une adresse deja vue par le GPU)
        ++g_prof_misses;
    }
    g_cur_tex = tex;
    g_cur_w = tex->w;
    g_cur_h = tex->h;
    GX_LoadTexObj(&tex->obj, g_bind_map);
    g_tex_dirty = false;
    return true;
}

// Bascule entre rendu sans texture (couleur du sommet) et avec texture (couleur du sommet x texel).
static void SetTexMode(bool on) {
    if (on == g_textured_mode) return;
    g_textured_mode = on;
    if (on) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        TevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    } else {
        GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        TevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    }
}

// Traduit le combiner RDP en mode 1 cycle (cycle 0 : (A - B) * C + D) vers un étage TEV GX, dans le cas courant B = 0
// (soit A * C + D, ce que GX sait faire : out = d + c*b + (1-c)*a avec a = 0, b = A). PRIM -> registre 0, ENV -> registre 1.
// Renvoie false si le combiner n'est pas représentable (le rendu retombe alors sur couleur_sommet x texel).
// Mode 2 cycles : classe le cycle 1 quand la couleur est un passe-plat ((ENV - COMBINED) * ENV_ALPHA + COMBINED avec ENV_ALPHA = 0) :
//   0 = autre (non pris en charge), 1 = alpha = COMBINED (passe-plat), 2 = alpha = COMBINED * PRIM_ALPHA (fondu des particules : fumee, poussiere).
// Le cycle 0 se traduit alors comme un combiner 1 cycle, suivi si besoin d'un etage multipliant l'alpha par PRIM_ALPHA.
static int CombinerCycle1Kind(u32 w0, u32 w1, u32 env) {
    (void)env;
    const u32 a1 = (w0 >> 5) & 0xF, c1 = w0 & 0x1F;
    const u32 b1 = (w1 >> 24) & 0xF, d1 = (w1 >> 6) & 7, aa1 = (w1 >> 21) & 7, ac1 = (w1 >> 18) & 7, ab1 = (w1 >> 3) & 7, ad1 = w1 & 7;
    // eau simple (G_CC_MODULATEIDECALA2) : couleur cycle 1 = COMBINED * SHADE, alpha = COMBINED
    if (a1 == 0 && c1 == 4 && d1 == 7 && b1 >= 8 && aa1 == 7 && ac1 == 7 && ab1 == 7 && ad1 == 0) return 3;
    // couleur : (ENV - COMBINED) * ALPHA_ENV + COMBINED (identite si l'alpha ENV vaut 0, melange vers ENV sinon)
    if (!(a1 == 5 && b1 == 0 && c1 == 12 && d1 == 0 && ab1 == 7)) return 0;
    if (aa1 == 7 && ac1 == 7 && ad1 == 0) return 1;
    if (aa1 == 0 && ac1 == 3 && ad1 == 7) return 2;
    return 0;
}
static bool g_cycle1_shade_mul = false;  // cycle 1 : couleur COMBINED * SHADE (eau simple)
static bool g_cycle1_env_lerp = false;   // couleur du cycle 1 : melange vers ENV selon l'alpha ENV (non nul)
static bool g_cycle1_prim_alpha = false; // ApplyCombiner1Cycle ajoute l'etage alpha * PRIM_ALPHA

// Traduit le combiner RDP en mode 1 cycle (cycle 0 : (A - B) * C + D) vers un étage TEV GX, dans le cas courant B = 0
// (soit A * C + D, ce que GX sait faire : out = d + c*b + (1-c)*a avec a = 0, b = A). PRIM -> registre 0, ENV -> registre 1.
// Renvoie false si le combiner n'est pas représentable (le rendu retombe alors sur couleur_sommet x texel).
// Mode 2 cycles dont le cycle 1 ne change rien : couleur (ENV - COMBINED) * ENV_ALPHA + COMBINED avec ENV_ALPHA = 0, alpha = COMBINED.
// Le cycle 0 se traduit alors comme un combiner 1 cycle (fumee/poussiere : texel * PRIM, alpha = texel_alpha * PRIM_alpha).
static bool CombinerCycle1IsPassthrough(u32 w0, u32 w1, u32 env) {
    const u32 c1 = w0 & 0x1F;
    const u32 d1 = (w1 >> 6) & 7, aa1 = (w1 >> 21) & 7, ac1 = (w1 >> 18) & 7, ab1 = (w1 >> 3) & 7, ad1 = w1 & 7;
    return c1 == 12 && d1 == 0 && (env & 0xFF) == 0 && aa1 == 7 && ac1 == 7 && ab1 == 7 && ad1 == 0;
}

static bool ApplyCombiner1Cycle(u32 w0, u32 w1, u32 prim, u32 env) {
    const u32 a = (w0 >> 20) & 0xF, c = (w0 >> 15) & 0x1F, b = (w1 >> 28) & 0xF, d = (w1 >> 15) & 7;
    const u32 aa = (w0 >> 12) & 7, ac = (w0 >> 9) & 7, ab = (w1 >> 12) & 7, ad = (w1 >> 9) & 7;
    // Cas gérés : B = 0 (A*C + D), ou B = D (mélange (A-B)*C + B = lerp(B, A, C), ex. G_CC_BLENDT_ENV_ALPHA_A_TxP du texte).
    const bool b_zero = b >= 8;
    const bool b_eq_d = !b_zero && b == d && (b == 1 || b == 3 || b == 4 || b == 5);
    if ((!b_zero && !b_eq_d) || ab != 7) return false;

    u8 ca, cc, cd, aA, aC, aD;
    switch (a) { case 1: ca = GX_CC_TEXC; break; case 3: ca = GX_CC_C0; break; case 4: ca = GX_CC_RASC; break; case 5: ca = GX_CC_C1; break;
                 case 6: ca = GX_CC_ONE; break; default: if (a >= 8) { ca = GX_CC_ZERO; break; } return false; }
    switch (c) { case 1: cc = GX_CC_TEXC; break; case 3: cc = GX_CC_C0; break; case 4: cc = GX_CC_RASC; break; case 5: cc = GX_CC_C1; break;
                 case 8: cc = GX_CC_TEXA; break; case 10: cc = GX_CC_A0; break; case 11: cc = GX_CC_RASA; break; case 12: cc = GX_CC_A1; break;
                 default: if (c >= 16) { cc = GX_CC_ZERO; break; } return false; }
    switch (d) { case 1: cd = GX_CC_TEXC; break; case 3: cd = GX_CC_C0; break; case 4: cd = GX_CC_RASC; break; case 5: cd = GX_CC_C1; break;
                 case 6: cd = GX_CC_ONE; break; case 7: cd = GX_CC_ZERO; break; default: return false; }
    bool alpha_one = false;
    switch (aa) { case 1: aA = GX_CA_TEXA; break; case 3: aA = GX_CA_A0; break; case 4: aA = GX_CA_RASA; break; case 5: aA = GX_CA_A1; break;
                  case 6: aA = GX_CA_KONST; alpha_one = true; break; case 7: aA = GX_CA_ZERO; break; default: return false; }
    switch (ac) { case 1: aC = GX_CA_TEXA; break; case 3: aC = GX_CA_A0; break; case 4: aC = GX_CA_RASA; break; case 5: aC = GX_CA_A1; break;
                  case 7: aC = GX_CA_ZERO; break; default: return false; }
    switch (ad) { case 1: aD = GX_CA_TEXA; break; case 3: aD = GX_CA_A0; break; case 4: aD = GX_CA_RASA; break; case 5: aD = GX_CA_A1; break;
                  case 6: aD = GX_CA_KONST; alpha_one = true; break; case 7: aD = GX_CA_ZERO; break; default: return false; }

    GXColor p0 = {static_cast<u8>(prim >> 24), static_cast<u8>(prim >> 16), static_cast<u8>(prim >> 8), static_cast<u8>(prim)};
    GXColor e0 = {static_cast<u8>(env >> 24), static_cast<u8>(env >> 16), static_cast<u8>(env >> 8), static_cast<u8>(env)};
    GX_SetTevColor(GX_TEVREG0, p0);
    GX_SetTevColor(GX_TEVREG1, e0);
    if (alpha_one) GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
    TevStages(1);
    u8 cb_enum = GX_CC_ZERO;
    if (b_eq_d) { cb_enum = (b == 1) ? GX_CC_TEXC : (b == 3) ? GX_CC_C0 : (b == 4) ? GX_CC_RASC : GX_CC_C1; cd = GX_CC_ZERO; }
    GX_SetTevColorIn(GX_TEVSTAGE0, cb_enum, ca, cc, cd);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, aA, aC, aD);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    if (g_cycle1_shade_mul) {
        TevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    } else     if (g_cycle1_prim_alpha || g_cycle1_env_lerp) { // cycle 1 : couleur = lerp vers ENV (alpha ENV) et/ou alpha = alpha precedent * PRIM_ALPHA
        TevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        if (g_cycle1_env_lerp) GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_C1, GX_CC_A1, GX_CC_ZERO); // (1 - A1) * CPREV + A1 * ENV
        else GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        if (g_cycle1_prim_alpha) GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_A0, GX_CA_ZERO);
        else GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    return true;
}

static bool TexBind(u32 tile_index) {
    const u64 t0 = gettime();
    const bool ok = TexBindImpl(tile_index);
    g_prof_tex += gettime() - t0;
    ++g_prof_binds;
    return ok;
}
