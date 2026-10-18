// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Polygones homogenes : decoupage (near/guard), lot de triangles GX, subdivision arete par arete.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

// Polygone en coordonnées homogènes (avant division perspective), pour le clipping contre le plan proche.
struct PolyVtx {
    f32 x, y, z, w;
    f32 r, g, b, a;
    f32 u, v;
};

static inline PolyVtx LerpPoly(const PolyVtx& a, const PolyVtx& b, f32 t) {
    PolyVtx o;
    o.x = a.x + (b.x - a.x) * t;
    o.y = a.y + (b.y - a.y) * t;
    o.z = a.z + (b.z - a.z) * t;
    o.w = a.w + (b.w - a.w) * t;
    o.r = a.r + (b.r - a.r) * t;
    o.g = a.g + (b.g - a.g) * t;
    o.b = a.b + (b.b - a.b) * t;
    o.a = a.a + (b.a - a.a) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    return o;
}

// Sutherland-Hodgman contre z + w >= 0 (plan proche N64) puis w >= epsilon. Renvoie le nombre de sommets (max 5).
// Decoupe homogene sur un plan x/w ou y/w = +-guard (guard-band) : evite des coordonnees ecran gigantesques (sommets proches du plan w ~ 0),
// que le rasteriseur GX arrondit mal (ciel / sol qui « tremblent »). axis 0 = x, 1 = y ; sign = +1 ou -1.
static int ClipPolyGuard(const PolyVtx* in, int n, PolyVtx* out, int axis, f32 sign, f32 guard) {
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const PolyVtx& a = in[i];
        const PolyVtx& b = in[(i + 1) % n];
        const f32 ca = axis == 0 ? a.x : a.y, cb = axis == 0 ? b.x : b.y;
        const f32 da = guard * a.w - sign * ca, db = guard * b.w - sign * cb; // >= 0 : a l'interieur
        if (da >= 0.0F) out[k++] = a;
        if ((da >= 0.0F) != (db >= 0.0F)) out[k++] = LerpPoly(a, b, da / (da - db));
    }
    return k;
}

static int ClipPolyNear(const PolyVtx* in, int n, PolyVtx* out) {
    PolyVtx tmp[16], tmp2[16];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const PolyVtx& a = in[i];
        const PolyVtx& b = in[(i + 1) % n];
        const f32 da = a.z + a.w, db = b.z + b.w;
        if (da >= 0.0F) tmp[m++] = a;
        if ((da >= 0.0F) != (db >= 0.0F)) tmp[m++] = LerpPoly(a, b, da / (da - db));
    }
    int k = 0;
    const f32 eps = g_near_w;
    for (int i = 0; i < m; ++i) {
        const PolyVtx& a = tmp[i];
        const PolyVtx& b = tmp[(i + 1) % m];
        const f32 da = a.w - eps, db = b.w - eps;
        if (da >= 0.0F) tmp2[k++] = a;
        if ((da >= 0.0F) != (db >= 0.0F)) tmp2[k++] = LerpPoly(a, b, da / (da - db));
    }
    if (g_guard_band <= 0.0F || k < 3) {
        for (int i = 0; i < k; ++i) out[i] = tmp2[i];
        return k;
    }
    PolyVtx* src = tmp2;
    PolyVtx* dst = tmp;
    int cnt = k;
    for (int plane = 0; plane < 4 && cnt >= 3; ++plane) {
        cnt = ClipPolyGuard(src, cnt, dst, plane >> 1, (plane & 1) ? -1.0F : 1.0F, g_guard_band);
        PolyVtx* t = src; src = dst; dst = t;
    }
    for (int i = 0; i < cnt; ++i) out[i] = src[i];
    return cnt;
}

// Émet un triangle (coordonnées homogènes déjà découpées contre le plan proche). GX ne reçoit que des positions déjà divisées par w :
// il interpole donc les coordonnées de texture linéairement à l'écran, sans correction de perspective, ce qui étire les textures des
// grands polygones proches de la caméra (sols, murs). On subdivise ces triangles : chacun, plus petit, est presque plan.
// Profondeur : GX attend z_clip dans [-1, 0] (-1 = proche, 0 = loin) mais coupe aux frontieres EXACTES sur materiel reel
// (les sommets a z = 0 ou -1, ex. le ciel / les rectangles, disparaissaient : image "en echo" car le Z n'etait jamais efface).
// On reste donc a l'interieur du volume de vue.
static int g_subdiv_max = 5; static f32 g_subdiv_ratio = 1.3F; // finesse de la subdivision (perspective) ; plus fine pour les decals (logo au sol)
static f32 g_zbias = 0.0F;   // decalage de profondeur (vers la camera) pour le mode « decal » du RDP (ZMODE = 3) : logo Nintendo au sol, ombres
static const f32 kZGeoNear = -0.9999F;
static const f32 kZGeoFar = -0.0003F;
static const f32 k2D_Z = -0.0001F; // rectangles 2D et effacement du Z : plus loin que toute geometrie

// Tampon de sommets GX pour regrouper les appels GX_Begin(GX_TRIANGLES)
// Évite d'émettre des milliers de couples GX_Begin / GX_End par image.
struct BatchVtx {
    f32 x, y, z;
    u8 r, g, b, a;
    u8 fog;
    f32 u, v;
};
constexpr u32 kBatchVtxCap = 1024;
static BatchVtx g_batch_vtx[kBatchVtxCap];
static u32 g_batch_vtx_n = 0;

static inline void FlushBatchTriangles(bool use_tex) {
    if (g_batch_vtx_n == 0) return;
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(g_batch_vtx_n));
    for (u32 i = 0; i < g_batch_vtx_n; ++i) {
        const BatchVtx& bv = g_batch_vtx[i];
        GX_Position3f32(bv.x, bv.y, bv.z);
        GX_Color4u8(bv.r, bv.g, bv.b, bv.a);
        if (g_fog_active) GX_Color4u8(255, 255, 255, bv.fog);
        if (use_tex) GX_TexCoord2f32(bv.u, bv.v);
    }
    GX_End();
    g_batch_vtx_n = 0;
}

static void EmitTriToBatch(const PolyVtx& a, const PolyVtx& b, const PolyVtx& c, bool use_tex) {
    if (g_batch_vtx_n + 3 > kBatchVtxCap) {
        FlushBatchTriangles(use_tex);
    }
    const PolyVtx* v[3] = {&a, &b, &c};
    for (int k = 0; k < 3; ++k) {
        const f32 iw = 1.0F / v[k]->w;
        f32 d = v[k]->z * iw * 0.5F + 0.5F;
        if (d < 0.0F) ++g_zclamp_lo; else if (d > 1.0F) ++g_zclamp_hi;
        if (g_zw_k > 0.0F) d = v[k]->w / (v[k]->w + g_zw_k);
        d = d < 0.0F ? 0.0F : (d > 1.0F ? 1.0F : d);
        BatchVtx& bv = g_batch_vtx[g_batch_vtx_n++];
        bv.x = v[k]->x * iw;
        bv.y = v[k]->y * iw;
        bv.z = kZGeoNear + d * (kZGeoFar - kZGeoNear) - g_zbias;
        bv.r = static_cast<u8>(v[k]->r);
        bv.g = static_cast<u8>(v[k]->g);
        bv.b = static_cast<u8>(v[k]->b);
        bv.a = static_cast<u8>(v[k]->a);
        if (g_fog_active) { // facteur de brouillard du RSP : alpha = clamp(z_ndc * fm + fo, 0, 255), nul si le sommet est derriere la camera
            const f32 f = v[k]->w > 0.0F ? (v[k]->z * iw) * g_fog_fm + g_fog_fo : 0.0F;
            bv.fog = static_cast<u8>(f < 0.0F ? 0.0F : (f > 255.0F ? 255.0F : f));
        }
        if (use_tex) {
            bv.u = v[k]->u;
            bv.v = v[k]->v;
        }
    }
}

static void DrawSubdividedTri(const PolyVtx& a, const PolyVtx& b, const PolyVtx& c, int level, bool use_tex) {
    const PolyVtx* v[3] = {&a, &b, &c};
    f32 nx[3], ny[3];
    f32 wmin = 1.0e30F, wmax = 0.0F;
    bool all_left = true, all_right = true, all_up = true, all_down = true;
    for (int k = 0; k < 3; ++k) {
        const f32 iw = 1.0F / v[k]->w;
        nx[k] = v[k]->x * iw;
        ny[k] = v[k]->y * iw;
        if (v[k]->w < wmin) wmin = v[k]->w;
        if (v[k]->w > wmax) wmax = v[k]->w;
        all_left = all_left && nx[k] < -1.2F;
        all_right = all_right && nx[k] > 1.2F;
        all_up = all_up && ny[k] > 1.2F;
        all_down = all_down && ny[k] < -1.2F;
    }
    if (all_left || all_right || all_up || all_down) return; // entièrement hors écran
    if (!g_cfg_subdiv || level >= g_subdiv_max) { EmitTriToBatch(a, b, c, use_tex); return; }
    if (g_edge_subdiv) {
        // Decoupage ARETE par ARETE : une arete est coupee si le rapport des w de ses deux extremites depasse le seuil. La decision ne depend que
        // de l'arete, donc deux triangles voisins coupent leur arete commune de la meme facon (pas de couture de texture). Cas 0 a 3 aretes coupees.
        auto split = [&](const PolyVtx& p, const PolyVtx& q) {
            const f32 lo = p.w < q.w ? p.w : q.w, hi = p.w < q.w ? q.w : p.w;
            return hi > g_subdiv_ratio * lo;
        };
        const bool s0 = split(a, b), s1 = split(b, c), s2 = split(c, a);
        const int n = (s0 ? 1 : 0) + (s1 ? 1 : 0) + (s2 ? 1 : 0);
        if (n == 0) { EmitTriToBatch(a, b, c, use_tex); return; }
        const PolyVtx ab = s0 ? LerpPoly(a, b, 0.5F) : a, bc = s1 ? LerpPoly(b, c, 0.5F) : b, ca = s2 ? LerpPoly(c, a, 0.5F) : c;
        const int l = level + 1;
        if (n == 3) {
            DrawSubdividedTri(a, ab, ca, l, use_tex); DrawSubdividedTri(ab, b, bc, l, use_tex);
            DrawSubdividedTri(ca, bc, c, l, use_tex); DrawSubdividedTri(ab, bc, ca, l, use_tex);
        } else if (n == 1) {
            if (s0) { DrawSubdividedTri(a, ab, c, l, use_tex); DrawSubdividedTri(ab, b, c, l, use_tex); }
            else if (s1) { DrawSubdividedTri(b, bc, a, l, use_tex); DrawSubdividedTri(bc, c, a, l, use_tex); }
            else { DrawSubdividedTri(c, ca, b, l, use_tex); DrawSubdividedTri(ca, a, b, l, use_tex); }
        } else { // deux aretes coupees : le sommet commun est isole, le quadrilatere restant est divise par sa diagonale la plus courte
            if (!s0) { // a-b entiere : coupees bc et ca ; sommet commun c
                DrawSubdividedTri(c, ca, bc, l, use_tex); DrawSubdividedTri(a, b, bc, l, use_tex); DrawSubdividedTri(a, bc, ca, l, use_tex);
            } else if (!s1) { // coupees ab et ca ; sommet commun a
                DrawSubdividedTri(a, ab, ca, l, use_tex); DrawSubdividedTri(ab, b, c, l, use_tex); DrawSubdividedTri(ab, c, ca, l, use_tex);
            } else { // coupees ab et bc ; sommet commun b
                DrawSubdividedTri(b, bc, ab, l, use_tex); DrawSubdividedTri(a, ab, c, l, use_tex); DrawSubdividedTri(ab, bc, c, l, use_tex);
            }
        }
        return;
    }
    if (wmax > g_subdiv_ratio * wmin) {
        const PolyVtx ab = LerpPoly(a, b, 0.5F), bc = LerpPoly(b, c, 0.5F), ca = LerpPoly(c, a, 0.5F);
        DrawSubdividedTri(a, ab, ca, level + 1, use_tex);
        DrawSubdividedTri(ab, b, bc, level + 1, use_tex);
        DrawSubdividedTri(ca, bc, c, level + 1, use_tex);
        DrawSubdividedTri(ab, bc, ca, level + 1, use_tex);
        return;
    }
    EmitTriToBatch(a, b, c, use_tex);
}
