// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Interpreteur de display list F3DDKR/RDP (ParseN64DisplayList) : un bloc par opcode.
// Fichier inclus par null_renderer.cpp (meme unite de compilation : les variables 'static' restent communes).

// Opcodes qui ne font que memoriser un etat RDP (combiner, type de cycle, othermode, couleurs prim/env/brouillard).
static void OpTrackState(u8 opcode, u32 w0, u32 w1) {
if (opcode == 0xFC) {
    g_last_combine_w0 = w0;
    g_last_combine_w1 = w1;
} else if (opcode == 0xBA && ((w0 >> 8) & 0xFF) == 20 && (w0 & 0xFF) == 2) { // gDPSetCycleType
    g_cycle_type = (w1 >> 20) & 3;
} else if (opcode == 0xEF) { // G_RDPSETOTHERMODE : le type de cycle est dans le mot haut
    g_cycle_type = (w0 >> 20) & 3;
    g_othermode_l = w1;
} else if (opcode == 0xB9) { // G_SETOTHERMODE_L : w0 = B9<<24 | sft<<8 | len, données alignées dans w1
    const u32 sft = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
    if (len > 0 && sft + len <= 32) {
        const u32 mask = (len == 32) ? 0xFFFFFFFFU : (((1U << len) - 1U) << sft);
        g_othermode_l = (g_othermode_l & ~mask) | (w1 & mask);
    }
} else if (opcode == 0xFA) { // G_SETPRIMCOLOR
    g_prim_color = w1;
} else if (opcode == 0xFB) { // G_SETENVCOLOR
    g_env_color = w1;
} else if (opcode == 0xF8) { // G_SETFOGCOLOR
    g_fog_rgb = w1;
    g_fog_have = true;
}
}

// G_DMAOFFSETS : decalages de base des matrices et des sommets.
static void OpDmaOffsets(u32 w0, u32 w1) {
    g_matrix_offset = w0 & 0x00FFFFFF;
    g_vertex_offset = w1 & 0x00FFFFFF;
}

// F3DDKR G_MTX : charge une matrice MVP 4x4 (16.16) dans l'emplacement 0/1/2 et la rend courante.
static void OpMatrix(u32 w0, u32 w1, std::uint8_t* rdram) {
    SeqPush('M');
    const u32 idx = std::min<u32>((w0 >> 22) & 0x3, 2);
    const u32 mtx_addr = (g_matrix_offset + (w1 & 0x00FFFFFF)) & 0x00FFFFFF;
    if (mtx_addr + 64 <= 0x00800000) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const int elem = r * 4 + c;
                const s16 int_val = ReadS16BE(rdram, mtx_addr + elem * 2);
                const u16 frac_val = static_cast<u16>(ReadS16BE(rdram, mtx_addr + 32 + elem * 2));
                g_mvp[idx][r][c] = static_cast<f32>(int_val) + static_cast<f32>(frac_val) / 65536.0F;
            }
        }
        // Charger une matrice la rend courante, pour les trois emplacements : mtx_cam_push (objets, emplacement 1) et
        // render_sprite_billboard (roues/hélices, emplacement 2) n'émettent aucun G_MW_MVPMATRIX après gSPMatrixDKR, alors que
        // mtx_head_push (emplacement 2) re-sélectionne explicitement l'emplacement 1 ensuite.
        g_cur_mvp = idx;
        ++g_mtx_loads[idx];
        if (g_cfg_ring) RingLog("MTX idx=%u addr=%06X\n", idx, mtx_addr);
        if (g_cfg_ring && idx == 2) RingLog("MTX2 d=%.4f,%.4f t=%.4f,%.4f,%.4f,%.4f r0=%.4f,%.4f r1=%.4f,%.4f\n",
                                            static_cast<double>(g_mvp[2][0][0]), static_cast<double>(g_mvp[2][1][1]),
                                            static_cast<double>(g_mvp[2][3][0]), static_cast<double>(g_mvp[2][3][1]), static_cast<double>(g_mvp[2][3][2]), static_cast<double>(g_mvp[2][3][3]),
                                            static_cast<double>(g_mvp[2][0][0]), static_cast<double>(g_mvp[2][0][1]), static_cast<double>(g_mvp[2][1][0]), static_cast<double>(g_mvp[2][1][1]));
    }
}

// G_MOVEWORD : selection de la MVP courante, billboard, position du brouillard.
static void OpMoveWord(u32 w0, u32 w1) {
    // gMoveWd(pkt, index, offset, data) : w0 = BC<<24 | offset<<8 | index (index dans l'octet de poids faible)
    const u32 index = w0 & 0xFF;
    if (index == 0x0A) { // G_MW_MVPMATRIX : sélection de la matrice courante
        g_cur_mvp = std::min<u32>(w1 >> 6, 2);
        if (g_cfg_ring) RingLog("SEL mvp=%u\n", g_cur_mvp);
        ++g_mtx_sels[g_cur_mvp];
    } else if (index == 0x02) { // G_MW_BILLBOARD
        g_billboard = (w1 & 1) != 0;
    } else if (index == 0x08) { // G_MW_FOG : gSPFogPosition(min, max) -> fm = 128000 / (max - min), fo = (500 - min) * 256 / (max - min)
        g_fog_fm = static_cast<f32>(static_cast<s16>(w1 >> 16));
        g_fog_fo = static_cast<f32>(static_cast<s16>(w1 & 0xFFFF));
    }
}

// F3DDKR G_VTX : transforme les sommets par la MVP courante (mode append / billboard : ajoute le sommet d'ancrage).
static void OpVertex(u32 w0, u32 w1, std::uint8_t* rdram) {
    SeqPush('V');
    const u32 count = ((w0 >> 19) & 0x1F) + 1;
    const bool append = ((w0 >> 16) & 1) != 0;
    // append : les sommets sont charges a partir de l'emplacement 1 (l'emplacement 0 garde le sommet d'ancrage du billboard) ; les indices
    // des triangles sont relatifs a ce chargement. Une 2e commande append (6e bande des plantes : 4 sommets) recharge donc l'emplacement 1,
    // et non a la suite du premier lot (sinon les triangles reutilisent les sommets de la bande precedente). cfg vtxappend=1 : ancien comportement.
    const u32 dst = append ? ((g_billboard && !g_cfg_vtxappend_old) ? 1U : g_vtx_count) : 0U; // billboard (plantes) : on repart de l'emplacement 1 ; personnages (sommets de transition du cou, mvp=2) : a la suite
    const u32 source = (g_vertex_offset + (w1 & 0x00FFFFFF)) & 0x00FFFFFF;

    if (g_cfg_drawinfo >= 2 && append && g_vtxlog_n < 3000) { // diagnostic : chargements de sommets en mode append
        {
            DbgLog( "dl=%u w0=%08X mvp=%u bb=%u count=%u cumul=%u src=%06X\n", g_dl_processed, w0, g_cur_mvp, g_billboard ? 1U : 0U, count, g_vtx_count, source);
            
        }
        ++g_vtxlog_n;
    }
    if (g_cfg_ring) RingLog("VTX w0=%08X count=%u app=%u cumul=%u mvp=%u bb=%u src=%06X cc=%08X:%08X om=%08X cyc=%u prim=%08X env=%08X\n", w0, count, append ? 1U : 0U, g_vtx_count, g_cur_mvp, g_billboard ? 1U : 0U, source, g_last_combine_w0, g_last_combine_w1, g_othermode_l, g_cycle_type, g_prim_color, g_env_color);
    g_dbg_vtx[0] = count; g_dbg_vtx[1] = append ? 1U : 0U; g_dbg_vtx[2] = dst; g_dbg_vtx[3] = source; g_dbg_vtx[4] = (dst + count <= 32) ? 0U : 1U; g_dbg_vtx[5] = g_billboard ? 1U : 0U; g_dbg_w0 = w0; ++g_dbg_vtx_n;
    if (dst + count <= 32 && source + count * 10 <= 0x00800000) {
        const f32 (*m)[4] = g_mvp[g_cur_mvp];
        for (u32 v = 0; v < count; ++v) {
            const u32 addr = source + v * 10;
            const f32 x = static_cast<f32>(ReadS16BE(rdram, addr + 0));
            const f32 y = static_cast<f32>(ReadS16BE(rdram, addr + 2));
            const f32 z = static_cast<f32>(ReadS16BE(rdram, addr + 4));
            ClipVertex& cv = g_clip[dst + v];
            g_vtx_slot_mvp[dst + v] = static_cast<u8>(g_cur_mvp);
            cv.x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
            cv.y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
            cv.z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
            cv.w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
            if (g_billboard && append) { // ajouté au sommet d'ancrage avant la division
                cv.x += g_clip[0].x; cv.y += g_clip[0].y; cv.z += g_clip[0].z; cv.w += g_clip[0].w;
                // Sprites de HUD (projection orthogonale : w constant = 160) : l'ancre est donnee en x/160 et y/160 alors que l'ecran N64 mesure 160 x 120 demi-pixels ;
                // la position verticale doit etre mise a l'echelle 160/120 pour retrouver le pixel pour pixel (sinon le ballon est plus bas que le texte).
                if (g_hud_yscale != 1.0F && g_clip[0].w > 159.9F && g_clip[0].w < 160.1F && std::fabs(g_clip[0].z) < 0.001F) cv.y *= g_hud_yscale;
            }
            cv.r = ReadU8BE(rdram, addr + 6);
            cv.g = ReadU8BE(rdram, addr + 7);
            cv.b = ReadU8BE(rdram, addr + 8);
            cv.a = ReadU8BE(rdram, addr + 9);
        }
        if (g_cfg_ring && g_billboard && append) { // diagnostic billboard : sommet d'ancrage (emplacement 0) et coins apres addition
            char bb[120];
            std::snprintf(bb, sizeof(bb), "BB n=%u anc=%.3f,%.3f,%.3f,%.3f", count, static_cast<double>(g_clip[0].x), static_cast<double>(g_clip[0].y), static_cast<double>(g_clip[0].z), static_cast<double>(g_clip[0].w));
            RingLog("%s\n", bb);
            for (u32 v = 0; v < count && v < 4; ++v) {
                const ClipVertex& q = g_clip[dst + v];
                RingLog("BBv%u raw=%d,%d,%d clip=%.3f,%.3f ndc=%.4f,%.4f\n", v, static_cast<int>(ReadS16BE(rdram, source + v * 10)), static_cast<int>(ReadS16BE(rdram, source + v * 10 + 2)),
                        static_cast<int>(ReadS16BE(rdram, source + v * 10 + 4)), static_cast<double>(q.x), static_cast<double>(q.y),
                        static_cast<double>(q.w != 0.0F ? q.x / q.w : 0.0F), static_cast<double>(q.w != 0.0F ? q.y / q.w : 0.0F));
            }
        }
        g_vtx_count = dst + count;
        g_vtx_by_slot[g_cur_mvp] += count;
    }
}

// Diagnostic : compte les triangles dessines par combiner (objets, matrice 1).
static void TriCombineStats(u32 count) {
    // Seulement les triangles dessinés avec la matrice d'objet (emplacement 1) : personnages/véhicules
    u32 slot = 0;
    while (g_cur_mvp == 1 && slot < g_combine_freq_n &&
           (g_combine_freq[slot].w0 != g_last_combine_w0 ||
            g_combine_freq[slot].w1 != g_last_combine_w1 ||
            g_combine_freq[slot].prim != g_prim_color ||
            g_combine_freq[slot].env != g_env_color)) ++slot;
    if (g_cur_mvp != 1) {
    } else if (slot < g_combine_freq_n) {
        g_combine_freq[slot].count += count;
    } else if (g_combine_freq_n < 16) {
        g_combine_freq[slot] = {g_last_combine_w0, g_last_combine_w1,
                                g_prim_color, g_env_color, count};
        ++g_combine_freq_n;
    }
}

// Diagnostic : statistiques des surfaces translucides (FORCE_BL) par combiner.
static void TriBlendDiag(u32 w0, u32 count, u32 source, std::uint8_t* rdram) {
        u32 bi = 0;
        const u32 tex_flag = (w0 >> 16) & 1U;
        u32 amin = 255, amax = 0;
        for (u32 t = 0; t < count; ++t) {
            const u32 ta = source + t * 16;
            for (int k = 0; k < 3; ++k) {
                const u8 vi = ReadU8BE(rdram, ta + 1 + k);
                if (vi >= 32) continue;
                amin = std::min<u32>(amin, g_clip[vi].a); amax = std::max<u32>(amax, g_clip[vi].a);
            }
        }
        while (bi < g_blend_freq_n && !(g_blend_freq[bi].w0 == g_last_combine_w0 && g_blend_freq[bi].w1 == g_last_combine_w1 &&
                                        g_blend_freq[bi].oml == g_othermode_l && g_blend_freq[bi].cyc == g_cycle_type && g_blend_freq[bi].tex == tex_flag)) ++bi;
        if (bi < g_blend_freq_n) {
            g_blend_freq[bi].count += count; g_blend_freq[bi].prim = g_prim_color; g_blend_freq[bi].env = g_env_color;
            g_blend_freq[bi].amin = std::min(g_blend_freq[bi].amin, amin); g_blend_freq[bi].amax = std::max(g_blend_freq[bi].amax, amax);
        } else if (g_blend_freq_n < 24) g_blend_freq[g_blend_freq_n++] = {g_last_combine_w0, g_last_combine_w1, g_othermode_l, g_cycle_type, g_prim_color, g_env_color, count, tex_flag, amin, amax};
}

static bool s_obj_shade_active = false; // le dernier dessin a utilise des etages TEV non standard (a remettre a 1 etage au dessin suivant)

// Materiel des modeles d'objets (personnages / vehicules) : combiner FC1596Ax:30FDFE38, 2 etages TEV.
static void TriObjShadeTev(u32 cc_w0) {
        const bool c1_shade = (cc_w0 & 0x1F) == 4;
        // Étage 0 : texel * alpha_sommet + couleur_sommet
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASA, GX_CC_RASC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        // Étage 1 : mélange du résultat vers la couleur ENV, facteur = SHADE ou TEXEL0
        GXColor env = {static_cast<u8>(g_env_color >> 24), static_cast<u8>(g_env_color >> 16),
                       static_cast<u8>(g_env_color >> 8), static_cast<u8>(g_env_color)};
        GX_SetTevColor(GX_TEVREG0, env);
        TevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_C0, c1_shade ? GX_CC_RASC : GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        s_obj_shade_active = true;
}

// Materiel de la surface de l'eau : deux textures (TEXEL0/TEXEL1 melangees par SHADE), 3 etages TEV.
static void TriWaterTev(bool& generic_tev) {
        // Surface de l'eau : cycle 0 couleur = lerp(TEXEL0, TEXEL1, SHADE) (deux textures animees), alpha = lerp(alpha TEXEL0, 1, alpha PRIM) ; cycle 1 : alpha x alpha SHADE.
        u8 water_tc1 = GX_TEXCOORD0;
        GxTex* keep = g_cur_tex; const u16 kw = g_cur_w, kh = g_cur_h;
        g_bind_map = GX_TEXMAP1;
        TexBind(1);
        g_bind_map = GX_TEXMAP0;
        const u16 w1 = g_cur_w, h1 = g_cur_h; // dimensions de la 2e texture (tuile 1)
        g_cur_tex = keep; g_cur_w = kw; g_cur_h = kh;
        // Les deux tuiles lisent les memes coordonnees en TEXELS (s,t du sommet) ; les textures n'ont pas la meme taille (16x16 animee et 128x128 vagues) :
        // la 2e se repete donc w0/w1 fois plus souvent. Le sommet transporte u0 = s / w0 : la 2e texture lit u1 = u0 * (w0 / w1) (decalages de tuile 2^-shift inclus).
        if (g_cfg_water2 && w1 != 0 && h1 != 0) {
            const f32 sh_s = static_cast<f32>(1 << g_tiles[1].shifts) / static_cast<f32>(1 << g_tiles[0].shifts); // tuile = coordonnee >> shift
            const f32 sh_t = static_cast<f32>(1 << g_tiles[1].shiftt) / static_cast<f32>(1 << g_tiles[0].shiftt);
            Mtx tm;
            std::memset(tm, 0, sizeof(tm));
            tm[0][0] = (static_cast<f32>(kw) / static_cast<f32>(w1)) / sh_s;
            tm[1][1] = (static_cast<f32>(kh) / static_cast<f32>(h1)) / sh_t;
            GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX2x4);
            GX_SetNumTexGens(2);
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
            GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
            water_tc1 = GX_TEXCOORD1;
        }
        GXColor p0 = {static_cast<u8>(g_prim_color >> 24), static_cast<u8>(g_prim_color >> 16), static_cast<u8>(g_prim_color >> 8), static_cast<u8>(g_prim_color)};
        GX_SetTevColor(GX_TEVREG0, p0);
        GXColor e0w = {static_cast<u8>(g_env_color >> 24), static_cast<u8>(g_env_color >> 16), static_cast<u8>(g_env_color >> 8), static_cast<u8>(g_env_color)};
        GX_SetTevColor(GX_TEVREG1, e0w);
        GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
        TevStages(3);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevOrder(GX_TEVSTAGE1, water_tc1, GX_TEXMAP1, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);   // (1 - SHADE) * TEXEL0 + SHADE * TEXEL1
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_APREV, GX_CA_KONST, GX_CA_A0, GX_CA_ZERO);   // (1 - PRIMa) * aT0 + PRIMa
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_CPREV, GX_CC_C1, GX_CC_A1, GX_CC_ZERO);                // cycle 1 : (ENV - COMBINED) x alpha ENV + COMBINED (teinte de l'eau du niveau)
        GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);  // alpha x alpha SHADE
        GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        generic_tev = true;
        s_obj_shade_active = true;
}

// Materiel des traces de pneus : sans texture, SHADE -> ENV puis x PRIM, 2 etages TEV.
static void TriTyreTev(bool& use_tex, bool& generic_tev) {
        // Traces de pneus (particules ligne, matrice 0) : cycle 0 couleur = lerp(SHADE, ENV, alpha ENV), alpha = alpha SHADE (sommets) ; cycle 1 = x PRIM (couleur et alpha).
        // Sans texture. Le fondu progressif le long du ruban vient de l'alpha des sommets et de l'alpha primitif.
        use_tex = false;
        SetTexMode(false);
        GXColor p0 = {static_cast<u8>(g_prim_color >> 24), static_cast<u8>(g_prim_color >> 16), static_cast<u8>(g_prim_color >> 8), static_cast<u8>(g_prim_color)};
        GXColor e0 = {static_cast<u8>(g_env_color >> 24), static_cast<u8>(g_env_color >> 16), static_cast<u8>(g_env_color >> 8), static_cast<u8>(g_env_color)};
        GX_SetTevColor(GX_TEVREG0, p0);
        GX_SetTevColor(GX_TEVREG1, e0);
        TevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_C1, GX_CC_A1, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C0, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_A0, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        generic_tev = true;
        s_obj_shade_active = true;
}

// Option wrapfix : les textures wrap dont les UV restent dans [0,1] sont dessinees en clamp. Renvoie vrai si l'objet de texture a ete remplace.
static bool TriWrapClamp(bool use_tex, u32 count, u32 source, f32 inv_w, f32 inv_h, std::uint8_t* rdram) {
    bool clamp_variant = false;
    if (use_tex && g_cfg_wrapfix && g_cur_tex != nullptr) {
        f32 umin = 1.0e9F, umax = -1.0e9F, vmin = 1.0e9F, vmax = -1.0e9F;
        for (u32 t = 0; t < count; ++t) {
            const u32 addr = source + t * 16;
            for (int k = 0; k < 3; ++k) {
                const f32 uu = static_cast<f32>(ReadS16BE(rdram, addr + 4 + k * 4)) * inv_w;
                const f32 vv = static_cast<f32>(ReadS16BE(rdram, addr + 6 + k * 4)) * inv_h;
                if (uu < umin) umin = uu;
                if (uu > umax) umax = uu;
                if (vv < vmin) vmin = vv;
                if (vv > vmax) vmax = vv;
            }
        }
        if (umin >= -g_wrap_margin && umax <= 1.0F + g_wrap_margin && vmin >= -g_wrap_margin && vmax <= 1.0F + g_wrap_margin) {
            GXTexObj clamped = g_cur_tex->obj;
            GX_InitTexObjWrapMode(&clamped, GX_CLAMP, GX_CLAMP);
            GX_LoadTexObj(&clamped, GX_TEXMAP0);
            clamp_variant = true;
        }
    }
    return clamp_variant;
}

// Diagnostics drawinfo=2 : une ligne de journal par lot (surfaces translucides sans texture, bandes de plantes).
static void TriDrawInfoDiag(bool use_tex, bool blend_on, u32 count, u32 source, std::uint8_t* rdram) {
    if (g_cfg_drawinfo >= 2 && !use_tex && blend_on && g_drawinfo_flat_n < 6000) { // surfaces translucides sans texture (traces de pneus, ombres)
        f32 xmin = 1e9F, xmax = -1e9F, ymin = 1e9F, ymax = -1e9F, wmin = 1e9F, wmax = -1e9F;
        u32 amin = 255, amax = 0, rmax = 0, gmax = 0, bmax = 0, rmin = 255, gmin = 255, bmin = 255;
        for (u32 t = 0; t < count; ++t) {
            const u32 addr = source + t * 16;
            for (int k = 0; k < 3; ++k) {
                const u8 vi = ReadU8BE(rdram, addr + 1 + k);
                if (vi >= 32) continue;
                const ClipVertex& c = g_clip[vi];
                const f32 iw = c.w != 0.0F ? 1.0F / c.w : 0.0F;
                xmin = std::min(xmin, c.x * iw); xmax = std::max(xmax, c.x * iw);
                ymin = std::min(ymin, c.y * iw); ymax = std::max(ymax, c.y * iw);
                wmin = std::min(wmin, c.w); wmax = std::max(wmax, c.w);
                amin = std::min<u32>(amin, c.a); amax = std::max<u32>(amax, c.a);
                rmin = std::min<u32>(rmin, c.r); rmax = std::max<u32>(rmax, c.r);
                gmin = std::min<u32>(gmin, c.g); gmax = std::max<u32>(gmax, c.g);
                bmin = std::min<u32>(bmin, c.b); bmax = std::max<u32>(bmax, c.b);
            }
        }
        if (!(wmin > 159.0F && wmax < 161.0F)) {
            DbgLog( "dl=%u tris=%u mvp=%u bb=%u xy=[%.2f..%.2f, %.2f..%.2f] w=[%.1f..%.1f] rgb=[%u..%u,%u..%u,%u..%u] a=[%u..%u] cc=%08X:%08X om_l=%08X cyc=%u prim=%08X\n",
                         g_dl_processed, count, g_cur_mvp, g_billboard ? 1U : 0U, static_cast<double>(xmin), static_cast<double>(xmax), static_cast<double>(ymin), static_cast<double>(ymax),
                         static_cast<double>(wmin), static_cast<double>(wmax), rmin, rmax, gmin, gmax, bmin, bmax, amin, amax,
                         g_last_combine_w0, g_last_combine_w1, g_othermode_l, g_cycle_type, g_prim_color);
            
        }
        ++g_drawinfo_flat_n;
    }
    if (g_cfg_drawinfo >= 2 && use_tex && g_cur_tex != nullptr && g_cur_w >= 48 && g_cur_w <= 124 && g_cur_h <= 28 && g_drawinfo_n < 1500) { // diagnostic (Dolphin) : une ligne par commande de triangles texturee
        f32 xmin = 1e9F, xmax = -1e9F, ymin = 1e9F, ymax = -1e9F, umin = 1e9F, umax = -1e9F, vmin = 1e9F, vmax = -1e9F;
        for (u32 t = 0; t < count; ++t) {
            const u32 addr = source + t * 16;
            for (int k = 0; k < 3; ++k) {
                const u8 vi = ReadU8BE(rdram, addr + 1 + k);
                if (vi >= 32) continue;
                const ClipVertex& c = g_clip[vi];
                const f32 iw = c.w != 0.0F ? 1.0F / c.w : 0.0F;
                xmin = std::min(xmin, c.x * iw); xmax = std::max(xmax, c.x * iw);
                ymin = std::min(ymin, c.y * iw); ymax = std::max(ymax, c.y * iw);
                const f32 uu = static_cast<f32>(ReadS16BE(rdram, addr + 4 + k * 4)) / 32.0F;
                const f32 vv = static_cast<f32>(ReadS16BE(rdram, addr + 6 + k * 4)) / 32.0F;
                umin = std::min(umin, uu); umax = std::max(umax, uu); vmin = std::min(vmin, vv); vmax = std::max(vmax, vv);
            }
        }
        {
            DbgLog( "vtx[w0=%08X n=%u count=%u app=%u dst=%u src=%06X skip=%u bb=%u] vcount=%u vi0=%u,%u,%u dl=%u tex#%u %ux%u tris=%u mvp=%u xy=[%.3f..%.3f, %.3f..%.3f] st=[%.1f..%.1f, %.1f..%.1f] cc=%08X:%08X om_l=%08X cyc=%u\n",
                         g_dbg_w0, g_dbg_vtx_n, g_dbg_vtx[0], g_dbg_vtx[1], g_dbg_vtx[2], g_dbg_vtx[3], g_dbg_vtx[4], g_dbg_vtx[5], g_vtx_count, static_cast<u32>(ReadU8BE(rdram, source + 1)), static_cast<u32>(ReadU8BE(rdram, source + 2)), static_cast<u32>(ReadU8BE(rdram, source + 3)), g_dl_processed, g_cur_tex->id, g_cur_w, g_cur_h, count, g_cur_mvp,
                         static_cast<double>(xmin), static_cast<double>(xmax), static_cast<double>(ymin), static_cast<double>(ymax),
                         static_cast<double>(umin), static_cast<double>(umax), static_cast<double>(vmin), static_cast<double>(vmax),
                         g_last_combine_w0, g_last_combine_w1, g_othermode_l, g_cycle_type);
            
        }
        ++g_drawinfo_n;
    }
}

// Biais de profondeur du mode decal et niveau de subdivision du lot (qualite dynamique).
static void TriSetupSubdiv() {
    g_zbias = (((g_othermode_l >> 10) & 3U) == 3U) ? g_decal_bias : 0.0F;
    {
        const bool decal = ((g_othermode_l >> 10) & 3U) == 3U;
        int base_subdiv = decal ? g_decal_subdiv : 3;
        f32 base_ratio = decal ? 1.08F : 1.3F;
        if (g_cfg_dynqual && !decal) {
            // Si la dernière frame a pris plus de 28 ms (~1.5 Mio de ticks sur Broadway 54 MHz de timer)
            // ou si la display list courante a déjà accumulé beaucoup de triangles (> 2200),
            // on réduit la subdivision pour préserver les 30 FPS constants.
            const u64 kBudgetTicks = 1520000ULL; // ~28 ms
            if (g_last_frame_ticks > kBudgetTicks || g_triangles_rendered > 2200) {
                base_subdiv = 1;
                base_ratio = 1.6F;
            } else if (g_triangles_rendered > 1500) {
                base_subdiv = 2;
                base_ratio = 1.4F;
            }
        }
        g_subdiv_max = base_subdiv;
        g_subdiv_ratio = base_ratio;
    }
}

// Brouillard du RDP : ajoute l'etage TEV (1-f)*couleur + f*brouillard et le canal couleur 1 si le mode de rendu est FOG_SHADE_A. Renvoie vrai si actif.
static bool TriFogBegin() {
    const bool fog_cc = g_cfg_fog && g_fog_have && g_fog_fm != 0.0F && ((g_othermode_l >> 30) & 3U) == 3U && ((g_othermode_l >> 26) & 3U) == 2U &&
                        ((g_othermode_l >> 22) & 3U) == 0U && ((g_othermode_l >> 18) & 3U) == 0U;
    g_fog_active = fog_cc;
    if (fog_cc) {
        const u8 st = static_cast<u8>(GX_TEVSTAGE0 + g_tev_n);
        TevStages(static_cast<u8>(g_tev_n + 1));
        GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1);
        GX_SetTevColorIn(st, GX_CC_CPREV, GX_CC_C2, GX_CC_RASA, GX_CC_ZERO);
        GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXColor fc = {static_cast<u8>(g_fog_rgb >> 24), static_cast<u8>(g_fog_rgb >> 16), static_cast<u8>(g_fog_rgb >> 8), 255};
        GX_SetTevColor(GX_TEVREG2, fc);
        GX_SetNumChans(2);
        GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    }
    return fog_cc;
}

// Parametres du materiel choisi pour un lot de triangles (consommes par la boucle de dessin).
struct TriMaterial {
    bool use_tex, obj_shade, generic_tev, blend_on, skip_cc, shade_alpha_cc;
    f32 inv_w, inv_h, half_u, half_v;
};

// Boucle de dessin d'un lot G_TRIN : culling, coordonnees de texture, clipping near, subdivision, envoi des triangles a GX.
static void TriDrawLoop(const TriMaterial& m, u32 count, u32 source, std::uint8_t* rdram) {
    const bool use_tex = m.use_tex, obj_shade = m.obj_shade, generic_tev = m.generic_tev, blend_on = m.blend_on, skip_cc = m.skip_cc, shade_alpha_cc = m.shade_alpha_cc;
    const f32 inv_w = m.inv_w, inv_h = m.inv_h, half_u = m.half_u, half_v = m.half_v;
    for (u32 t = 0; t < count; ++t) {
        const u32 addr = source + t * 16;
        const u8 vi[3] = {ReadU8BE(rdram, addr + 1), ReadU8BE(rdram, addr + 2), ReadU8BE(rdram, addr + 3)};
        if (g_cfg_drawinfo && g_stale_n < 400 && (vi[0] >= g_vtx_count || vi[1] >= g_vtx_count || vi[2] >= g_vtx_count)) { // sommets jamais charges par le dernier lot : indices suspects
            DbgLog("STALE dl=%u mvp=%u bb=%u vi=%u,%u,%u vcount=%u last_vtx[w0=%08X count=%u app=%u dst=%u]\n", g_dl_processed, g_cur_mvp, g_billboard ? 1U : 0U,
                   static_cast<u32>(vi[0]), static_cast<u32>(vi[1]), static_cast<u32>(vi[2]), g_vtx_count, g_dbg_w0, g_dbg_vtx[0], g_dbg_vtx[1], g_dbg_vtx[2]);
            ++g_stale_n;
        }
        if (skip_cc) continue;
        if (vi[0] >= 32 || vi[1] >= 32 || vi[2] >= 32) { if (g_cfg_ring) RingLog("DROP idx>=32 vi=%u,%u,%u\n", static_cast<u32>(vi[0]), static_cast<u32>(vi[1]), static_cast<u32>(vi[2])); continue; }
        const ClipVertex* cv[3] = {&g_clip[vi[0]], &g_clip[vi[1]], &g_clip[vi[2]]};
        if (g_cfg_cull != 0 && (ReadU8BE(rdram, addr) & 0x40U) == 0) {
            // Orientation = signe du determinant des lignes (x, y, w) : valable meme si un sommet est derriere la camera.
            const f32 det = cv[0]->x * (cv[1]->y * cv[2]->w - cv[2]->y * cv[1]->w) -
                            cv[0]->y * (cv[1]->x * cv[2]->w - cv[2]->x * cv[1]->w) +
                            cv[0]->w * (cv[1]->x * cv[2]->y - cv[2]->x * cv[1]->y);
            if (g_cfg_cull > 0 ? det <= 0.0F : det >= 0.0F) {
                ++g_tri_culled;
                if (g_cfg_ring) {
                    const f32 i0 = cv[0]->w != 0.0F ? 1.0F / cv[0]->w : 0.0F, i1 = cv[1]->w != 0.0F ? 1.0F / cv[1]->w : 0.0F, i2 = cv[2]->w != 0.0F ? 1.0F / cv[2]->w : 0.0F;
                    RingLog("DROP cull ndc=%.2f,%.2f %.2f,%.2f %.2f,%.2f mvp=%u\n", static_cast<double>(cv[0]->x * i0), static_cast<double>(cv[0]->y * i0), static_cast<double>(cv[1]->x * i1),
                            static_cast<double>(cv[1]->y * i1), static_cast<double>(cv[2]->x * i2), static_cast<double>(cv[2]->y * i2), g_cur_mvp);
                }
                continue;
            }
        }
        if (g_cfg_ring) {
            const ClipVertex* q[3] = {&g_clip[vi[0] & 31], &g_clip[vi[1] & 31], &g_clip[vi[2] & 31]};
            f32 ax[3], ay[3];
            for (int k = 0; k < 3; ++k) { const f32 iw = q[k]->w != 0.0F ? 1.0F / q[k]->w : 0.0F; ax[k] = q[k]->x * iw; ay[k] = q[k]->y * iw; }
            RingLog("T t%u %u,%u,%u m%u%u%u %.1f,%.1f %.1f,%.1f %.1f,%.1f%s a=%u,%u,%u c=%04X\n", (use_tex && g_cur_tex != nullptr) ? static_cast<u32>(g_cur_tex->id) : 0U, static_cast<u32>(vi[0]), static_cast<u32>(vi[1]), static_cast<u32>(vi[2]),
                    static_cast<u32>(g_vtx_slot_mvp[vi[0] & 31]), static_cast<u32>(g_vtx_slot_mvp[vi[1] & 31]), static_cast<u32>(g_vtx_slot_mvp[vi[2] & 31]),
                    static_cast<double>(ax[0]), static_cast<double>(ay[0]), static_cast<double>(ax[1]), static_cast<double>(ay[1]), static_cast<double>(ax[2]), static_cast<double>(ay[2]),
                    (vi[0] >= g_vtx_count || vi[1] >= g_vtx_count || vi[2] >= g_vtx_count) ? " STALE" : "", static_cast<u32>(q[0]->a), static_cast<u32>(q[1]->a), static_cast<u32>(q[2]->a), g_last_combine_w0 & 0xFFFFU);
        }
        PolyVtx pv[3];
        for (int k = 0; k < 3; ++k) {
            pv[k].x = cv[k]->x; pv[k].y = cv[k]->y; pv[k].z = cv[k]->z; pv[k].w = cv[k]->w;
            pv[k].r = cv[k]->r; pv[k].g = cv[k]->g; pv[k].b = cv[k]->b;
            const bool vertex_alpha = obj_shade || generic_tev || (blend_on && (g_cfg_vtxalpha || (!use_tex && g_cfg_flatalpha) || (g_cfg_shadealpha && shade_alpha_cc)));
            pv[k].a = vertex_alpha ? static_cast<f32>(cv[k]->a) : 255.0F; // l'alpha des sommets porte les fondus translucides
            // Le RDP filtre entre les texels floor(s) et floor(s)+1 (centres des texels aux coordonnees entieres) alors que GX
            // place les centres a +0,5 : decalage d'un demi-texel pour que le bord de texture (s = 0) tombe sur le texel 0 et ne se
            // melange pas avec le bord oppose (pointes de feuilles recopiees en haut des plantes).
            pv[k].u = use_tex ? static_cast<f32>(ReadS16BE(rdram, addr + 4 + k * 4)) * inv_w + half_u : 0.0F;
            pv[k].v = use_tex ? static_cast<f32>(ReadS16BE(rdram, addr + 6 + k * 4)) * inv_h + half_v : 0.0F;
        }
        PolyVtx poly[16];
        const int pn = ClipPolyNear(pv, 3, poly);
        if (pn < 3) {
            if (g_cfg_ring) {
                char s[100];
                std::snprintf(s, sizeof(s), "w=%.2f,%.2f,%.2f xy=%.0f,%.0f %.0f,%.0f %.0f,%.0f", static_cast<double>(pv[0].w), static_cast<double>(pv[1].w), static_cast<double>(pv[2].w),
                              static_cast<double>(pv[0].x), static_cast<double>(pv[0].y), static_cast<double>(pv[1].x), static_cast<double>(pv[1].y), static_cast<double>(pv[2].x), static_cast<double>(pv[2].y));
                RingLog("DROP near %s mvp=%u\n", s, g_cur_mvp);
            }
            continue;
        }
        for (int k = 1; k + 1 < pn; ++k) DrawSubdividedTri(poly[0], poly[k], poly[k + 1], 0, use_tex);
    }
}

// F3DDKR G_TRIN : choisit le materiel (combiner -> etages TEV), puis dessine les triangles.
static void OpTriangles(u32 w0, u32 w1, std::uint8_t* rdram) {
    SeqPush('T');
    ++g_tri_cmds; if (g_to_depth) ++g_tri_depth_skipped;
    const u32 count = ((w0 >> 20) & 0xF) + 1;
    const u32 source = w1 & 0x00FFFFFF;

    if (!g_to_depth && source + count * 16 <= 0x00800000) {
        g_triangles_rendered += count;
        TriCombineStats(count);
        // Mélange alpha (surfaces translucides : nuages, ombres douces...) si FORCE_BL est actif ; pas d'écriture Z dans ce cas
        const bool blend_on = ((g_othermode_l >> 14) & 1) != 0;
        // Ecriture de profondeur : les surfaces en melange alpha (FORCE_BL) n'ecrivent pas le Z, SAUF si le mode de rendu du jeu le demande (bit Z_UPD = 0x20 d'othermode_l) :
        // la dalle du hub est dessinee ainsi ; sans cela le decor dessine apres la recouvre (cfg zupd=0 : ancien comportement).
        const bool z_write = !blend_on || g_blend_zw || (g_cfg_zupd && (g_othermode_l & 0x20U) != 0U);
        GX_SetZMode(g_force_noz ? GX_FALSE : GX_TRUE, GX_LEQUAL, z_write ? GX_TRUE : GX_FALSE);
        if (blend_on) TriBlendDiag(w0, count, source, rdram);
        if (blend_on) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        // Bit 16 du premier mot : texture activée (gSPPolygon). Tuile de rendu 0.
        const bool tex_on = ((w0 >> 16) & 1) != 0;
        if (tex_on && g_tex_dirty) TexBind(0);
        bool use_tex = tex_on && g_cur_tex != nullptr;
        SetTexMode(use_tex);
        if (g_cfg_ring && use_tex) RingLog("TB t%u %ux%u cc=%08X:%08X\n", static_cast<u32>(g_cur_tex->id), static_cast<u32>(g_cur_w), static_cast<u32>(g_cur_h), g_last_combine_w0, g_last_combine_w1);
        // Combiner d'éclairage des modèles d'objets (personnages/véhicules) FC1596Ax:30FDFE38, décodé :
        //   cycle 0 couleur : (TEXEL0 - PRIM) * SHADE_ALPHA + SHADE   (PRIM noir -> texel * alpha_sommet + couleur_sommet)
        //   cycle 1 couleur : (ENV - COMBINED) * c1 + COMBINED, c1 = TEXEL0 (A1) ou SHADE (A4) -> mélange vers la couleur ENV
        //   alpha           : TEXEL0_ALPHA * PRIM_ALPHA                (PRIM_ALPHA = 255 pour les objets opaques)
        // TEV GX : out = d + (1-c)*a + c*b.
        const bool skip_cc = g_cfg_skipcc != 0 && (g_last_combine_w0 & 0xFFFFU) == g_cfg_skipcc; // diagnostic : ne dessine pas les triangles de ce combiner
        const u32 cc_w0 = g_last_combine_w0 & 0x00FFFFFFU;
        const bool obj_shade = use_tex && g_cur_mvp == 1 && (cc_w0 >> 9) == (0x001596A1U >> 9) &&
                               g_last_combine_w1 == 0x30FDFE38U && g_prim_color == 0x000000FFU;
        bool generic_tev = false;
        if (obj_shade) {
            TriObjShadeTev(cc_w0);
        } else if (use_tex && g_cycle_type == 1 && g_cfg_water && (g_last_combine_w0 & 0x00FFFFFFU) == 0x002266ACU && g_last_combine_w1 == 0x1010923FU) {
            TriWaterTev(generic_tev);
        } else if (g_cycle_type == 1 && (g_last_combine_w0 & 0x00FFFFFFU) == 0x00567E03U && g_last_combine_w1 == 0x4F0E79FFU) {
            TriTyreTev(use_tex, generic_tev);
        } else if (use_tex && (g_cycle1_kind = (g_cycle_type == 1 ? CombinerCycle1Kind(g_last_combine_w0, g_last_combine_w1, g_env_color) : 0),
                                 g_cycle1_prim_alpha = g_cycle1_kind == 2, g_cycle1_shade_mul = g_cycle1_kind == 3, g_cycle1_env_lerp = (g_cycle1_kind == 1 || g_cycle1_kind == 2) && (g_env_color & 0xFF) != 0, g_cycle_type == 0 || g_cycle1_kind != 0) &&
                   ApplyCombiner1Cycle(g_last_combine_w0, g_last_combine_w1, g_prim_color, g_env_color)) {
            // Combiner 1 cycle générique (A*C + D) : modulation par la couleur primitive/alpha, etc.
            generic_tev = true;
            s_obj_shade_active = true;
        } else if (s_obj_shade_active) {
            s_obj_shade_active = false;
            TevStages(1);
            GX_SetTevOp(GX_TEVSTAGE0, use_tex ? GX_MODULATE : GX_PASSCLR);
        }
        // Coordonnées de texture F3DDKR : s16 en 10.5 (1/32 de texel), normalisées par la taille
        // Surfaces translucides dont l'alpha du combiner (cycle 0) vient de l'alpha SHADE (alpha des sommets) : traces de pneus (particules ligne),
        // fondu progressif le long du ruban (cfg shadealpha=0 : ancien rendu, alpha force a 255).
        const bool shade_alpha_cc = (((g_last_combine_w0 >> 12) & 7) == 4) || (((g_last_combine_w0 >> 9) & 7) == 4) ||
                                    (((g_last_combine_w1 >> 12) & 7) == 4) || (((g_last_combine_w1 >> 9) & 7) == 4);
        const f32 inv_w = 1.0F / (32.0F * static_cast<f32>(g_cur_w));
        const f32 inv_h = 1.0F / (32.0F * static_cast<f32>(g_cur_h));
        // Textures en mode « wrap » dont les coordonnees restent dans [0, 1] : GX filtrerait les bords avec le bord oppose
        // (pointes de feuilles recopiees du cote oppose) ; on les dessine en « clamp » (cfg wrapfix=0 pour desactiver).
        const f32 half_u = (use_tex && g_cfg_texoff && g_cfg_texfilt && g_cur_tex != nullptr) ? 0.5F / static_cast<f32>(g_cur_w) : 0.0F;
        const f32 half_v = (use_tex && g_cfg_texoff && g_cfg_texfilt && g_cur_tex != nullptr) ? 0.5F / static_cast<f32>(g_cur_h) : 0.0F;
        const bool clamp_variant = TriWrapClamp(use_tex, count, source, inv_w, inv_h, rdram);
        TriDrawInfoDiag(use_tex, blend_on, count, source, rdram);
        TriSetupSubdiv();
        // Brouillard du RDP : melange de cycle 0 « FOG_SHADE_A » (P = couleur de brouillard, A = alpha SHADE, M = pixel, B = 1 - A) : l'alpha des sommets
        // y est le facteur de brouillard calcule par le RSP. Etage TEV supplementaire (1 - f) * couleur + f * brouillard, f lu sur le canal couleur 1.
        const u8 fog_base_stages = g_tev_n;
        const bool fog_cc = TriFogBegin();
        const TriMaterial mat{use_tex, obj_shade, generic_tev, blend_on, skip_cc, shade_alpha_cc, inv_w, inv_h, half_u, half_v};
        TriDrawLoop(mat, count, source, rdram);
        FlushBatchTriangles(use_tex);
        if (fog_cc) { // retire l'etage de brouillard et le canal couleur 1
            TevStages(fog_base_stages);
            GX_SetNumChans(1);
            GX_SetVtxDesc(GX_VA_CLR1, GX_NONE);
            g_fog_active = false;
        }
        if (clamp_variant && g_cur_tex != nullptr) GX_LoadTexObj(&g_cur_tex->obj, GX_TEXMAP0); // remet l'objet d'origine (wrap) pour les dessins suivants
        if (blend_on) GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
}

// G_FILLRECT dont l'image cible est le Z-buffer : efface la profondeur (quad profondeur seule).
static void OpFillRectDepth() {
    g_rects_skipped++;
    // Le jeu efface son Z-buffer : écrire la profondeur maximale sans toucher à la couleur
    {
        Mtx zid;
        guMtxIdentity(zid);
        GX_LoadPosMtxImm(zid, GX_PNMTX0);
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        SetTexMode(false);
        GX_SetColorUpdate(GX_FALSE);
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
            GX_Position3f32(-1.01F, 1.01F, k2D_Z);
            GX_Color4u8(0, 0, 0, 0xFF);
            GX_Position3f32(1.01F, 1.01F, k2D_Z);
            GX_Color4u8(0, 0, 0, 0xFF);
            GX_Position3f32(1.01F, -1.01F, k2D_Z);
            GX_Color4u8(0, 0, 0, 0xFF);
            GX_Position3f32(-1.01F, -1.01F, k2D_Z);
            GX_Color4u8(0, 0, 0, 0xFF);
        GX_End();
        GX_SetColorUpdate(GX_TRUE);
    }
}

// G_FILLRECT : rectangle plein (UI, fondus d'ecran).
static void OpFillRect(u32 w0, u32 w1) {
    SeqPush('R');
    g_rects_rendered++;
    const s32 lrx = static_cast<s32>((w0 >> 12) & 0xFFF) / 4;
    const s32 lry = static_cast<s32>(w0 & 0xFFF) / 4;
    const s32 ulx = static_cast<s32>((w1 >> 12) & 0xFFF) / 4;
    const s32 uly = static_cast<s32>(w1 & 0xFFF) / 4;

    const f32 x0 = (static_cast<f32>(ulx) / 160.0F) - 1.0F;
    const f32 x1 = (static_cast<f32>(lrx) / 160.0F) - 1.0F;
    const f32 y0 = 1.0F - (static_cast<f32>(uly) / 120.0F);
    const f32 y1 = 1.0F - (static_cast<f32>(lry) / 120.0F);

    g_dbg_rect[0] = ulx; g_dbg_rect[1] = uly; g_dbg_rect[2] = lrx; g_dbg_rect[3] = lry;
    g_dbg_rect[4] = (g_fill_r << 16) | (g_fill_g << 8) | g_fill_b;
    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    SetTexMode(false);

    // En mode FILL le rectangle prend la couleur de remplissage (opaque). En mode 1/2 cycles (fondus d'écran :
    // transition_render_fullscreen) il passe par le pipeline : couleur primitive, alpha primitif, mélange alpha.
    u8 cr = g_fill_r, cg = g_fill_g, cb = g_fill_b, ca = 0xFF;
    const bool pipeline = g_cycle_type <= 1;
    if (pipeline) {
        cr = static_cast<u8>(g_prim_color >> 24);
        cg = static_cast<u8>(g_prim_color >> 16);
        cb = static_cast<u8>(g_prim_color >> 8);
        ca = static_cast<u8>(g_prim_color);
        if (ca != 0xFF) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    }

    if ((lrx - ulx) >= 300 && (lry - uly) >= 220) { ++g_dbg_full_n; g_dbg_full_col = (static_cast<u32>(cr) << 24) | (static_cast<u32>(cg) << 16) | (static_cast<u32>(cb) << 8) | ca; g_dbg_full_cyc = g_cycle_type; }
    if (g_norect) goto rect_done;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(x0, y0, k2D_Z);
        GX_Color4u8(cr, cg, cb, ca);
        GX_Position3f32(x1, y0, k2D_Z);
        GX_Color4u8(cr, cg, cb, ca);
        GX_Position3f32(x1, y1, k2D_Z);
        GX_Color4u8(cr, cg, cb, ca);
        GX_Position3f32(x0, y1, k2D_Z);
        GX_Color4u8(cr, cg, cb, ca);
    GX_End();
rect_done:
    if (pipeline && ca != 0xFF) GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

// G_TEXRECT / G_TEXRECTFLIP : rectangle texture ; lit les 2 G_RDPHALF qui suivent (indices i+1 et i+2).
static void OpTexRect(u8 opcode, u32 w0, u32 w1, const u32* cmd_ptr, u32 cmd_count, u32 i) {
    SeqPush('X');
    g_rects_rendered++;
    // Mot 0 : lr (XH,YH) ; mot 1 : tuile + ul (XL,YL) ; puis G_RDPHALF_1 (s,t) et G_RDPHALF_2 (dsdx,dtdy)
    const u32 tile = (w1 >> 24) & 7;
    const f32 lrx = static_cast<f32>((w0 >> 12) & 0xFFF) / 4.0F;
    const f32 lry = static_cast<f32>(w0 & 0xFFF) / 4.0F;
    const f32 ulx = static_cast<f32>((w1 >> 12) & 0xFFF) / 4.0F;
    const f32 uly = static_cast<f32>(w1 & 0xFFF) / 4.0F;
    const u32 half1 = (i + 1 < cmd_count) ? cmd_ptr[(i + 1) * 2 + 1] : 0;
    const u32 half2 = (i + 2 < cmd_count) ? cmd_ptr[(i + 2) * 2 + 1] : 0;
    const f32 s0 = static_cast<f32>(static_cast<s16>(half1 >> 16)) / 32.0F;   // texels (10.5)
    const f32 t0 = static_cast<f32>(static_cast<s16>(half1 & 0xFFFF)) / 32.0F;
    const f32 dsdx = static_cast<f32>(static_cast<s16>(half2 >> 16)) / 1024.0F; // texels par pixel (5.10)
    const f32 dtdy = static_cast<f32>(static_cast<s16>(half2 & 0xFFFF)) / 1024.0F;
    const bool flip = (opcode == 0xE5);
    ++g_tr_count;
    g_tr_last[0] = ulx; g_tr_last[1] = uly; g_tr_last[2] = lrx; g_tr_last[3] = lry;

    const f32 x0 = (ulx / 160.0F) - 1.0F;
    const f32 x1 = (lrx / 160.0F) - 1.0F;
    const f32 y0 = 1.0F - (uly / 120.0F);
    const f32 y1 = 1.0F - (lry / 120.0F);

    Mtx id;
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    const bool use_tex = TexBind(tile) && g_cur_tex != nullptr;
    g_tr_last_i[0] = tile; g_tr_last_i[1] = g_tiles[tile].fmt; g_tr_last_i[2] = g_tiles[tile].siz;
    g_tr_last_i[3] = g_cur_w; g_tr_last_i[4] = g_cur_h; g_tr_last_i[5] = use_tex ? 1 : 0;
    g_tr_last_i[6] = static_cast<u32>(static_cast<s16>(half1 >> 16)); g_tr_last_i[7] = static_cast<u32>(static_cast<s16>(half1 & 0xFFFF));
    SetTexMode(use_tex);
    if (use_tex) {
        // Les rectangles texturés d'un même visuel (logo = 11 morceaux adjacents) ne doivent pas échantillonner au-delà
        // de leur texture : le bilinéaire + mode "wrap" ramène le bord opposé et trace de fines lignes. On force "clamp".
        GXTexObj clamped = g_cur_tex->obj;
        GX_InitTexObjWrapMode(&clamped, GX_CLAMP, GX_CLAMP);
        GX_LoadTexObj(&clamped, GX_TEXMAP0);
        g_tex_dirty = true; // la prochaine liaison (triangles) doit recharger l'objet d'origine
    }
    // Combiner 1 cycle (couleur/alpha primitifs : opacité et teinte du texte et des sprites) + mélange alpha du mode de rendu
    const bool rect_custom = use_tex && g_cycle_type == 0 &&
                             ApplyCombiner1Cycle(g_last_combine_w0, g_last_combine_w1, g_prim_color, g_env_color);
    const bool rect_blend = ((g_othermode_l >> 14) & 1) != 0;
    if (rect_blend) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    const f32 wpx = lrx - ulx, hpx = lry - uly;
    // Les coordonnées de texture s'incrémentent de dsdx par pixel en x (dtdy en y) ; FLIP échange s et t
    f32 u0 = s0, v0 = t0, u1, v1;
    if (!flip) { u1 = s0 + dsdx * wpx; v1 = t0 + dtdy * hpx; }
    else       { u1 = s0 + dtdy * hpx; v1 = t0 + dsdx * wpx; }
    const f32 iw = 1.0F / static_cast<f32>(g_cur_w), ih = 1.0F / static_cast<f32>(g_cur_h);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position3f32(x0, y0, k2D_Z);
        GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
        if (use_tex) GX_TexCoord2f32(u0 * iw, v0 * ih);
        GX_Position3f32(x1, y0, k2D_Z);
        GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
        if (use_tex) GX_TexCoord2f32((flip ? u0 : u1) * iw, (flip ? v1 : v0) * ih);
        GX_Position3f32(x1, y1, k2D_Z);
        GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
        if (use_tex) GX_TexCoord2f32(u1 * iw, v1 * ih);
        GX_Position3f32(x0, y1, k2D_Z);
        GX_Color4u8(0xFF, 0xFF, 0xFF, 0xFF);
        if (use_tex) GX_TexCoord2f32((flip ? u1 : u0) * iw, (flip ? v0 : v1) * ih);
    GX_End();
    if (rect_blend) GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    if (rect_custom) {
        TevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }
    // Sauter les 2 commandes G_RDPHALF qui suivent
}

static void ParseN64DisplayList(const u32* cmd_ptr, u32 cmd_count, std::uint8_t* rdram, int depth) {
    if (depth > 8) { ++g_dl_depth_exceeded; return; }
    if (cmd_ptr == nullptr || rdram == nullptr) return;

    for (u32 i = 0; i < cmd_count; ++i) {
        // Budget global par display list : évite les boucles/récursions sur des données invalides
        if (g_dl_budget == 0) { ++g_dl_budget_exhausted; return; }
        --g_dl_budget;
        const u32 w0 = cmd_ptr[i * 2];
        const u32 w1 = cmd_ptr[i * 2 + 1];
        const u8 opcode = static_cast<u8>(w0 >> 24);
        ++g_op_hist[opcode];
        OpTrackState(opcode, w0, w1);
        if (depth == 0) { g_root_ops[g_root_n & 63] = opcode; ++g_root_n; }
        g_dl_opcodes[g_dl_opcode_next] = opcode;
        g_dl_opcode_next = (g_dl_opcode_next + 1) % 24;
        if (g_dl_opcode_count < 24) ++g_dl_opcode_count;

        if (opcode == 0xBF) { // kDMAOffsetsOpcode
            OpDmaOffsets(w0, w1);
        } else if (opcode == 0x01) { // kMatrixOpcode
            OpMatrix(w0, w1, rdram);
        } else if (opcode == 0xBC) { // G_MOVEWORD
            OpMoveWord(w0, w1);
        } else if (opcode == 0x04) { // kVertexOpcode
            OpVertex(w0, w1, rdram);
        } else if (opcode == 0x05) { // kTriangleOpcode
            OpTriangles(w0, w1, rdram);
        } else if (opcode == 0x06) { // kDisplayListOpcode (G_DL)
            const bool branch = ((w0 >> 16) & 0x1) != 0;
            const u32 sub_dl = (w1 & 0x00FFFFFF) & ~7U;
            if (sub_dl + 8 <= 0x00800000) {
                ParseN64DisplayList(reinterpret_cast<const u32*>(rdram + sub_dl), 1024, rdram, depth + 1);
            } else {
                ++g_gdl_rejected;
                g_last_bad_gdl_w1 = w1;
            }
            if (branch) break;
        } else if (opcode == 0x07) { // kCountedDisplayListOpcode (G_DMADL)
            const u32 count = (w0 >> 16) & 0xFF;
            const u32 dma_addr = w1 & 0x00FFFFFF;
            ++g_dmadl_count;
            g_dmadl_max_count = std::max(g_dmadl_max_count, count);
            if (count > 0 && dma_addr + count * 8 <= 0x00800000) {
                ParseN64DisplayList(reinterpret_cast<const u32*>(rdram + dma_addr), count, rdram, depth + 1);
            } else {
                ++g_dmadl_rejected;
                g_last_bad_dmadl_w1 = w1;
            }
        } else if (opcode == 0xFD) { // G_SETTIMG
            TexSetTimg(w0, w1);
        } else if (opcode == 0xF5) { // G_SETTILE
            TexSetTile(w0, w1);
        } else if (opcode == 0xF2) { // G_SETTILESIZE
            TexSetTileSize(w0, w1);
        } else if (opcode == 0xF3) { // G_LOADBLOCK
            TexLoadBlock(rdram, w0, w1);
        } else if (opcode == 0xF4) { // G_LOADTILE
            TexLoadTile(rdram, w0, w1);
        } else if (opcode == 0xF0) { // G_LOADTLUT
            TexLoadTlut(rdram, w0, w1);
        } else if (opcode == 0xFE) { // G_SETZIMG
            g_zimg_addr = w1; // adresse segmentée complète (le segment fait partie de l'identité)
        } else if (opcode == 0xFF) { // G_SETCIMG
            SeqPush('P');
            g_cimg_last = w1; ++g_cimg_changes;
            g_to_depth = w1 == g_zimg_addr;
        } else if (opcode == 0xF7) { // G_SETFILLCOLOR : RGBA5551 (x2)
            const u32 c = w1 & 0xFFFF;
            g_fill_r = static_cast<u8>(((c >> 11) & 31) * 255 / 31);
            g_fill_g = static_cast<u8>(((c >> 6) & 31) * 255 / 31);
            g_fill_b = static_cast<u8>(((c >> 1) & 31) * 255 / 31);
        } else if (opcode == 0xF6 && g_to_depth) { // G_FILLRECT dans le Z-buffer
            OpFillRectDepth();
        } else if (opcode == 0xF6) { // kFillRectOpcode
            OpFillRect(w0, w1);
        } else if (opcode == 0xE4 || opcode == 0xE5) { // G_TEXRECT (0xE4) et G_TEXRECTFLIP (0xE5)
            OpTexRect(opcode, w0, w1, cmd_ptr, cmd_count, i);
            i += 2; // saute les 2 commandes G_RDPHALF qui suivent
        } else if (opcode == 0xB8) { // kEndDisplayListOpcode (G_ENDDL)
            break;
        }
    }
}
