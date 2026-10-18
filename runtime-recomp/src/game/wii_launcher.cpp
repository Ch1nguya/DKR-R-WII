// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Ecran de lancement du port Wii : s'affiche toujours au demarrage (comme les compilations remasterisees),
// permet de regler les controles, le filtrage des textures, le lissage de l'image et le son, puis lance le jeu.
//
// Rendu 100 % logiciel dans l'XFB (YUY2) : le lanceur s'execute avant l'initialisation de GX par le moteur, il ne partage donc
// aucun etat avec lui. Les reglages sont ecrits dans sd:/apps/DKR-R/dkr-wii.cfg (les lignes inconnues, p. ex. les options de
// diagnostic, sont conservees). `launcher=0` dans ce fichier saute l'ecran (tests automatises).
#if defined(GEKKO)

#include <gccore.h>
#include <wiiuse/wpad.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

extern void* g_wii_xfb;
extern GXRModeObj* g_wii_rmode;

namespace {

struct LauncherGlyph { int code, adv, w, h, ox, oy, off; };
#include "wii_launcher_font.inc"

struct Font {
    const LauncherGlyph* glyphs;
    const unsigned char* bits;
    int count, ascent, height;
};
const Font kSmall{kFontSmallGlyphs, kFontSmallBits, kFontSmallCount, kFontSmallAscent, kFontSmallHeight};
const Font kBig{kFontBigGlyphs, kFontBigBits, kFontBigCount, kFontBigAscent, kFontBigHeight};

bool g_inited = false;
int g_w = 640, g_h = 480;
std::uint32_t* g_frame = nullptr;  // 0x00RRGGBB
std::uint32_t* g_bg = nullptr;
int g_img_h = 360;

// ---------------------------------------------------------------- configuration
std::vector<std::string> g_lines;

void LoadCfgLines() {
    g_lines.clear();
    FILE* f = std::fopen("sd:/apps/DKR-R/dkr-wii.cfg", "r");
    if (f == nullptr) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) g_lines.push_back(s);
    }
    std::fclose(f);
}

void SaveCfgLines() {
    FILE* f = std::fopen("sd:/apps/DKR-R/dkr-wii.cfg", "w");
    if (f == nullptr) return;
    for (const std::string& s : g_lines) std::fprintf(f, "%s\n", s.c_str());
    std::fclose(f);
}

float GetCfg(const char* key, float def) {
    const size_t n = std::strlen(key);
    for (const std::string& s : g_lines) {
        if (s.size() > n && s.compare(0, n, key) == 0 && s[n] == '=') return static_cast<float>(std::atof(s.c_str() + n + 1));
    }
    return def;
}

void SetCfg(const char* key, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s=%g", key, static_cast<double>(v));
    const size_t n = std::strlen(key);
    for (std::string& s : g_lines) {
        if (s.size() > n && s.compare(0, n, key) == 0 && s[n] == '=') { s = buf; return; }
    }
    g_lines.emplace_back(buf);
}

enum Str {
    S_PLAY, S_OPTIONS, S_QUIT, S_HINT_MAIN, S_HINT_OPTS, S_RESET, S_BACK,
    S_O_LANG, S_O_ASPECT, S_O_VIDEO, S_O_TILT, S_O_RANGE, S_O_DEAD, S_O_INVERT, S_O_TEXFILT, S_O_SMOOTH, S_O_AUDIO, S_O_DIAG,
    S_ROM_OK, S_ROM_MISSING, S_ROM_BAD, S_ROM_CHECK,
    S_CONSOLE, S_OVERLAY,
    S_AUTO, S_STRETCHED, S_BARS43, S_V480I, S_V480P,
    S_NO, S_YES, S_SHARP, S_BILINEAR, S_OFF, S_NORMAL, S_STRONG, S_DISABLED, S_ENABLED, S_HIDDEN, S_SHOWN,
    S_COUNT
};
constexpr int kLangCount = 8;
const char* const kLangNames[kLangCount] = {"English", "Français", "Español", "Deutsch", "Italiano", "Português", "日本語", "한국어"};
const char* const kStrings[kLangCount][S_COUNT] = {
    {"Play", "Options", "Quit", "A: select     L/R: choose     +: play", "Up/Down: choose    L/R: change    B: back",
     "Reset to defaults", "Back", "Language", "Screen format", "Video mode", "Tilt steering (Wiimote)", "Tilt range", "Dead zone", "Invert tilt",
     "Texture filtering", "Image smoothing", "Sound", "Diagnostic info",
     "ROM OK", "ROM not found. Copy your Diddy Kong Racing ROM here:", "Unsupported ROM: only the two known US versions of Diddy Kong Racing are accepted", "Verifying ROM...",
     "Console", "Overlay",
     "Auto", "Stretched", "4:3 bars",
     "480i", "480p",
     "No", "Yes", "Sharp pixels", "Bilinear", "Off", "Normal", "Strong", "Disabled", "Enabled", "Hidden", "Shown"},
    {"Jouer", "Options", "Quitter", "A : valider     G/D : choisir     + : jouer", "Haut/Bas : choisir    G/D : modifier    B : retour",
     "Rétablir les valeurs par défaut", "Retour", "Langue", "Format d'écran", "Mode vidéo", "Pilotage par inclinaison (Wiimote)", "Amplitude d'inclinaison", "Zone morte", "Inverser l'inclinaison",
     "Filtrage des textures", "Lissage de l'image", "Son", "Informations de diagnostic",
     "ROM OK", "ROM introuvable. Copiez votre ROM de Diddy Kong Racing ici :", "ROM non prise en charge : seules les deux versions US connues de Diddy Kong Racing sont acceptées", "Vérification de la ROM...",
     "Console", "Superposition",
     "Auto", "Étiré", "4:3 (bandes)",
     "480i", "480p",
     "Non", "Oui", "Pixels nets", "Bilinéaire", "Désactivé", "Normal", "Fort", "Désactivé", "Activé", "Masquées", "Affichées"},
    {"Jugar", "Opciones", "Salir", "A: aceptar     I/D: elegir     +: jugar", "Arriba/Abajo: elegir    I/D: cambiar    B: volver",
     "Restablecer valores", "Volver", "Idioma", "Formato de pantalla", "Modo de vídeo", "Control por inclinación (Wiimote)", "Amplitud de inclinación", "Zona muerta", "Invertir inclinación",
     "Filtrado de texturas", "Suavizado de imagen", "Sonido", "Información de diagnóstico",
     "ROM correcta", "ROM no encontrada. Copia tu ROM de Diddy Kong Racing aquí:", "ROM no compatible: solo se aceptan las dos versiones US conocidas de Diddy Kong Racing", "Verificando la ROM...",
     "Consola", "Superpuesto",
     "Auto", "Estirado", "4:3 (barras)",
     "480i", "480p",
     "No", "Sí", "Píxeles nítidos", "Bilineal", "Desactivado", "Normal", "Fuerte", "Desactivado", "Activado", "Oculta", "Visible"},
    {"Spielen", "Optionen", "Beenden", "A: wählen     L/R: auswählen     +: spielen", "Hoch/Runter: wählen    L/R: ändern    B: zurück",
     "Standardwerte", "Zurück", "Sprache", "Bildformat", "Videomodus", "Neigungssteuerung (Wiimote)", "Neigungsbereich", "Totzone", "Neigung umkehren",
     "Texturfilter", "Bildglättung", "Ton", "Diagnoseinfos",
     "ROM OK", "ROM nicht gefunden. Kopiere deine Diddy-Kong-Racing-ROM hierhin:", "ROM nicht unterstützt: nur die beiden bekannten US-Versionen von Diddy Kong Racing werden akzeptiert", "ROM wird geprüft...",
     "Konsole", "Overlay",
     "Auto", "Gestreckt", "4:3 (Balken)",
     "480i", "480p",
     "Nein", "Ja", "Scharfe Pixel", "Bilinear", "Aus", "Normal", "Stark", "Aus", "Ein", "Versteckt", "Sichtbar"},
    {"Gioca", "Opzioni", "Esci", "A: conferma     S/D: scegli     +: gioca", "Su/Giù: scegli    S/D: cambia    B: indietro",
     "Ripristina predefiniti", "Indietro", "Lingua", "Formato schermo", "Modalità video", "Controllo a inclinazione (Wiimote)", "Ampiezza inclinazione", "Zona morta", "Inverti inclinazione",
     "Filtro texture", "Smussatura immagine", "Audio", "Info diagnostica",
     "ROM OK", "ROM non trovata. Copia la tua ROM di Diddy Kong Racing qui:", "ROM non supportata: sono accettate solo le due versioni US note di Diddy Kong Racing", "Verifica della ROM...",
     "Console", "Overlay",
     "Auto", "Esteso", "4:3 (bande)",
     "480i", "480p",
     "No", "Sì", "Pixel nitidi", "Bilineare", "Disattivato", "Normale", "Forte", "Disattivato", "Attivato", "Nascoste", "Visibili"},
    {"Jogar", "Opções", "Sair", "A: confirmar     E/D: escolher     +: jogar", "Cima/Baixo: escolher    E/D: alterar    B: voltar",
     "Restaurar padrões", "Voltar", "Idioma", "Formato do ecrã", "Modo de vídeo", "Controlo por inclinação (Wiimote)", "Amplitude de inclinação", "Zona morta", "Inverter inclinação",
     "Filtragem de texturas", "Suavização da imagem", "Som", "Informação de diagnóstico",
     "ROM OK", "ROM não encontrada. Copie a sua ROM de Diddy Kong Racing para:", "ROM não suportada: apenas as duas versões US conhecidas de Diddy Kong Racing são aceites", "A verificar a ROM...",
     "Consola", "Sobreposição",
     "Auto", "Esticado", "4:3 (barras)",
     "480i", "480p",
     "Não", "Sim", "Píxeis nítidos", "Bilinear", "Desativado", "Normal", "Forte", "Desativado", "Ativado", "Ocultas", "Visíveis"},
    {"プレイ", "オプション", "終了", "A: 決定     左右: 選択     +: プレイ", "上下: 選択    左右: 変更    B: 戻る",
     "初期設定に戻す", "戻る", "言語", "画面比率", "ビデオモード", "傾きで操作 (Wiimote)", "傾きの範囲", "遊び (不感帯)", "傾きを反転",
     "テクスチャフィルター", "画像のスムージング", "サウンド", "診断情報",
     "ROM OK", "ROMが見つかりません。Diddy Kong RacingのROMを次の場所にコピーしてください:", "非対応のROMです。既知の米国版Diddy Kong Racing(2種類)のみ使用できます", "ROMを確認中...",
     "コンソール", "オーバーレイ",
     "自動", "引き伸ばし", "4:3 (黒帯)",
     "480i", "480p",
     "いいえ", "はい", "シャープ", "バイリニア", "オフ", "標準", "強", "オフ", "オン", "非表示", "表示"},
    {"플레이", "옵션", "종료", "A: 선택     좌/우: 이동     +: 플레이", "상/하: 이동    좌/우: 변경    B: 뒤로",
     "기본값으로 되돌리기", "뒤로", "언어", "화면 비율", "비디오 모드", "기울기 조작 (Wiimote)", "기울기 범위", "데드존", "기울기 반전",
     "텍스처 필터링", "이미지 부드럽게", "사운드", "진단 정보",
     "ROM 확인됨", "ROM을 찾을 수 없습니다. Diddy Kong Racing ROM을 여기에 복사하세요:", "지원하지 않는 ROM입니다. 알려진 미국판 Diddy Kong Racing 2종만 사용할 수 있습니다", "ROM 확인 중...",
     "콘솔", "오버레이",
     "자동", "늘리기", "4:3 (검은 띠)",
     "480i", "480p",
     "아니요", "예", "선명하게", "쌍선형", "끔", "보통", "강하게", "끔", "켬", "숨김", "표시"},
};
int g_lang = 0;
inline const char* T(Str id) { return kStrings[g_lang][id]; }

struct Option {
    Str label;
    const char* key;
    float def, lo, hi, step;
    Str names[3]; // enumeration (S_COUNT = valeur numerique ; la langue est geree a part)
    const char* unit;
};
const Option kOptions[] = {
    {S_O_LANG, "lang", 0, 0, kLangCount - 1, 1, {S_COUNT, S_COUNT, S_COUNT}, ""}, // noms propres, voir FormatValue
    {S_O_ASPECT, "aspect", 0, 0, 2, 1, {S_AUTO, S_STRETCHED, S_BARS43}, ""},
    {S_O_VIDEO, "video", 0, 0, 2, 1, {S_AUTO, S_V480I, S_V480P}, ""},
    {S_O_TILT, "tilt", 1, 0, 1, 1, {S_NO, S_YES, S_COUNT}, ""},
    {S_O_RANGE, "range", 40, 20, 80, 5, {S_COUNT, S_COUNT, S_COUNT}, "°"},
    {S_O_DEAD, "dead", 3, 0, 10, 1, {S_COUNT, S_COUNT, S_COUNT}, "°"},
    {S_O_INVERT, "invert", 0, 0, 1, 1, {S_NO, S_YES, S_COUNT}, ""},
    {S_O_TEXFILT, "texfilt", 1, 0, 1, 1, {S_SHARP, S_BILINEAR, S_COUNT}, ""},
    {S_O_SMOOTH, "smooth", 1, 0, 2, 1, {S_OFF, S_NORMAL, S_STRONG}, ""},
    {S_O_AUDIO, "audio", 1, 0, 1, 1, {S_DISABLED, S_ENABLED, S_COUNT}, ""},
    {S_O_DIAG, "diag", 0, 0, 2, 1, {S_OFF, S_CONSOLE, S_OVERLAY}, ""},
    // Option "Ameliore" (mipmaps + filtrage anisotrope) retiree de l'interface pour la version alpha : effet faible, resultats non verifies sur console.
    // {S_O_DIAG, "enhanced", 0, 0, 1, 1, {S_OFF, S_ENABLED, S_COUNT}, ""},
};
constexpr int kOptionCount = static_cast<int>(sizeof(kOptions) / sizeof(kOptions[0]));
const char* OptionLabel(const Option& o) {
    static const char* const labels[kLangCount] = {
        "Enhanced", "Amélioré", "Mejorado", "Erweitert",
        "Migliorato", "Melhorado", "Enhanced", "Enhanced"
    };
    return std::strcmp(o.key, "enhanced") == 0 ? labels[g_lang] : T(o.label);
}

// ---------------------------------------------------------------- dessin
inline std::uint32_t Blend(std::uint32_t dst, std::uint32_t src, int a) { // a : 0..255
    const int ia = 255 - a;
    const std::uint32_t r = (((dst >> 16) & 255) * ia + ((src >> 16) & 255) * a) / 255;
    const std::uint32_t g = (((dst >> 8) & 255) * ia + ((src >> 8) & 255) * a) / 255;
    const std::uint32_t b = ((dst & 255) * ia + (src & 255) * a) / 255;
    return (r << 16) | (g << 8) | b;
}

void FillRect(int x0, int y0, int x1, int y1, std::uint32_t color, int alpha) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_w) x1 = g_w;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; ++y) {
        std::uint32_t* row = g_frame + static_cast<size_t>(y) * g_w;
        for (int x = x0; x < x1; ++x) row[x] = alpha >= 255 ? color : Blend(row[x], color, alpha);
    }
}

void FrameRect(int x0, int y0, int x1, int y1, int t, std::uint32_t color, int alpha) {
    FillRect(x0, y0, x1, y0 + t, color, alpha);
    FillRect(x0, y1 - t, x1, y1, color, alpha);
    FillRect(x0, y0 + t, x0 + t, y1 - t, color, alpha);
    FillRect(x1 - t, y0 + t, x1, y1 - t, color, alpha);
}

// UTF-8 -> points de code (suffisant pour le francais)
int NextCode(const char*& p) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) { const int v = ((c & 0x1F) << 6) | (static_cast<unsigned char>(*p) & 0x3F); ++p; return v; }
    if ((c & 0xF0) == 0xE0) {
        const int v = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(p[0]) & 0x3F) << 6) | (static_cast<unsigned char>(p[1]) & 0x3F);
        p += 2;
        return v;
    }
    return '?';
}

const LauncherGlyph* FindGlyph(const Font& f, int code) {
    for (int i = 0; i < f.count; ++i) if (f.glyphs[i].code == code) return &f.glyphs[i];
    return nullptr;
}

int TextWidth(const Font& f, const char* s) {
    int w = 0;
    while (*s) {
        const LauncherGlyph* g = FindGlyph(f, NextCode(s));
        if (g != nullptr) w += g->adv;
    }
    return w;
}

void DrawText(const Font& f, int x, int y_top, const char* s, std::uint32_t color, int alpha = 255) {
    const int base = y_top + f.ascent;
    while (*s) {
        const LauncherGlyph* g = FindGlyph(f, NextCode(s));
        if (g == nullptr) continue;
        for (int j = 0; j < g->h; ++j) {
            const int py = base + g->oy + j;
            if (py < 0 || py >= g_h) continue;
            for (int i = 0; i < g->w; ++i) {
                const int px = x + g->ox + i;
                if (px < 0 || px >= g_w) continue;
                const int a = f.bits[g->off + j * g->w + i] * alpha / 255;
                if (a > 0) g_frame[static_cast<size_t>(py) * g_w + px] = Blend(g_frame[static_cast<size_t>(py) * g_w + px], color, a);
            }
        }
        x += g->adv;
    }
}

void DrawTextCentered(const Font& f, int cx, int y_top, const char* s, std::uint32_t color, int alpha = 255) {
    DrawText(f, cx - TextWidth(f, s) / 2, y_top, s, color, alpha);
}

void BuildBackground() {
    g_bg = static_cast<std::uint32_t*>(std::malloc(static_cast<size_t>(g_w) * g_h * 4));
    if (g_bg == nullptr) return;
    g_img_h = g_w * 9 / 16;
    // degrade de secours (ciel) si l'image est absente
    for (int y = 0; y < g_h; ++y) {
        const std::uint32_t c = Blend(0x2E7DD8, 0x0A1E3C, y * 255 / g_h);
        for (int x = 0; x < g_w; ++x) g_bg[static_cast<size_t>(y) * g_w + x] = c;
    }
    FILE* f = std::fopen("sd:/apps/DKR-R/launcher.bin", "rb");
    if (f == nullptr) return;
    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f) == 12 && std::memcmp(hdr, "DKRL", 4) == 0) {
        const int iw = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
        const int ih = (hdr[8] << 24) | (hdr[9] << 16) | (hdr[10] << 8) | hdr[11];
        if (iw > 0 && ih > 0 && iw <= 1024 && ih <= 1024) {
            std::vector<unsigned char> rgb(static_cast<size_t>(iw) * ih * 3);
            if (std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size()) {
                const int out_h = g_img_h < g_h ? g_img_h : g_h;
                for (int y = 0; y < out_h; ++y) {
                    const unsigned char* src = &rgb[static_cast<size_t>(y * ih / out_h) * iw * 3];
                    for (int x = 0; x < g_w; ++x) {
                        const unsigned char* p = src + static_cast<size_t>(x * iw / g_w) * 3;
                        g_bg[static_cast<size_t>(y) * g_w + x] = (p[0] << 16) | (p[1] << 8) | p[2];
                    }
                }
                // sous l'image : prolonge la derniere ligne en fondu vers le bleu nuit
                for (int y = out_h; y < g_h; ++y) {
                    const int a = (y - out_h + 1) * 255 / (g_h - out_h + 1);
                    for (int x = 0; x < g_w; ++x) {
                        g_bg[static_cast<size_t>(y) * g_w + x] = Blend(g_bg[static_cast<size_t>(out_h - 1) * g_w + x], 0x0A1E3C, a);
                    }
                }
            }
        }
    }
    std::fclose(f);
}

void Present() {
    std::uint32_t* xfb = static_cast<std::uint32_t*>(g_wii_xfb); // YUY2 : Y0 Cb Y1 Cr par paire de pixels
    for (int y = 0; y < g_h; ++y) {
        const std::uint32_t* row = g_frame + static_cast<size_t>(y) * g_w;
        std::uint32_t* out = xfb + static_cast<size_t>(y) * (g_w / 2);
        for (int x = 0; x < g_w; x += 2) {
            const int r0 = (row[x] >> 16) & 255, g0 = (row[x] >> 8) & 255, b0 = row[x] & 255;
            const int r1 = (row[x + 1] >> 16) & 255, g1 = (row[x + 1] >> 8) & 255, b1 = row[x + 1] & 255;
            const int y0 = 16 + ((66 * r0 + 129 * g0 + 25 * b0) >> 8);
            const int y1 = 16 + ((66 * r1 + 129 * g1 + 25 * b1) >> 8);
            const int r = (r0 + r1) >> 1, g = (g0 + g1) >> 1, b = (b0 + b1) >> 1;
            const int cb = 128 + ((-38 * r - 74 * g + 112 * b) >> 8);
            const int cr = 128 + ((112 * r - 94 * g - 18 * b) >> 8);
            out[x >> 1] = (static_cast<std::uint32_t>(y0) << 24) | (static_cast<std::uint32_t>(cb) << 16) |
                          (static_cast<std::uint32_t>(y1) << 8) | static_cast<std::uint32_t>(cr);
        }
    }
}

// ---------------------------------------------------------------- entrees
struct Input { bool up, down, left, right, ok, back, start; };

Input g_prev_held{};
int g_hold_frames[4] = {};

Input PollInput() {
    PAD_ScanPads();
    WPAD_ScanPads();
    Input in{};
    bool held[4] = {}; // haut, bas, gauche, droite (instantane)
    bool ok = false, back = false, start = false;
    for (int i = 0; i < 4; ++i) {
        const std::uint16_t pb = PAD_ButtonsHeld(i);
        const int sy = PAD_StickY(i), sx = PAD_StickX(i);
        if ((pb & PAD_BUTTON_UP) || sy > 40) held[0] = true;
        if ((pb & PAD_BUTTON_DOWN) || sy < -40) held[1] = true;
        if ((pb & PAD_BUTTON_LEFT) || sx < -40) held[2] = true;
        if ((pb & PAD_BUTTON_RIGHT) || sx > 40) held[3] = true;
        if (PAD_ButtonsDown(i) & PAD_BUTTON_A) ok = true;
        if (PAD_ButtonsDown(i) & PAD_BUTTON_B) back = true;
        if (PAD_ButtonsDown(i) & PAD_BUTTON_START) start = true;

        u32 type = 0;
        if (WPAD_Probe(i, &type) != WPAD_ERR_NONE) continue;
        const u32 w = WPAD_ButtonsHeld(i);
        const u32 wd = WPAD_ButtonsDown(i);
        if (w & WPAD_BUTTON_HOME) { std::exit(0); }
        const WPADData* d = WPAD_Data(i);
        const bool upright = d != nullptr && d->exp.type == WPAD_EXP_NUNCHUK; // avec Nunchuk la telecommande est tenue droite
        if (upright) {
            if (w & WPAD_BUTTON_UP) held[0] = true;
            if (w & WPAD_BUTTON_DOWN) held[1] = true;
            if (w & WPAD_BUTTON_LEFT) held[2] = true;
            if (w & WPAD_BUTTON_RIGHT) held[3] = true;
        } else { // telecommande horizontale (comme en jeu) : haut = droite, bas = gauche, gauche = haut, droite = bas
            if (w & WPAD_BUTTON_RIGHT) held[0] = true;
            if (w & WPAD_BUTTON_LEFT) held[1] = true;
            if (w & WPAD_BUTTON_UP) held[2] = true;
            if (w & WPAD_BUTTON_DOWN) held[3] = true;
        }
        if (d != nullptr && d->exp.type == WPAD_EXP_NUNCHUK) {
            const int jy = static_cast<int>(d->exp.nunchuk.js.pos.y) - d->exp.nunchuk.js.center.y;
            const int jx = static_cast<int>(d->exp.nunchuk.js.pos.x) - d->exp.nunchuk.js.center.x;
            if (jy > 40) held[0] = true;
            if (jy < -40) held[1] = true;
            if (jx < -40) held[2] = true;
            if (jx > 40) held[3] = true;
        }
        if (wd & (WPAD_BUTTON_A | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_A)) ok = true;
        if (wd & (WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_B)) back = true;
        if (wd & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) start = true;
        if (w & WPAD_CLASSIC_BUTTON_UP) held[0] = true;
        if (w & WPAD_CLASSIC_BUTTON_DOWN) held[1] = true;
        if (w & WPAD_CLASSIC_BUTTON_LEFT) held[2] = true;
        if (w & WPAD_CLASSIC_BUTTON_RIGHT) held[3] = true;
    }
    bool* out[4] = {&in.up, &in.down, &in.left, &in.right};
    for (int k = 0; k < 4; ++k) { // front + repetition automatique
        if (held[k]) {
            ++g_hold_frames[k];
            if (g_hold_frames[k] == 1 || (g_hold_frames[k] > 22 && (g_hold_frames[k] % 5) == 0)) *out[k] = true;
        } else {
            g_hold_frames[k] = 0;
        }
    }
    in.ok = ok;
    in.back = back;
    in.start = start;
    return in;
}

// ---------------------------------------------------------------- ecrans
constexpr std::uint32_t kGold = 0xFFD83A, kWhite = 0xFFFFFF, kNavy = 0x0A1E3C, kGray = 0xB8C4D8;

void FormatValue(const Option& o, float v, char* out, size_t n) {
    int idx = static_cast<int>(v + 0.5F);
    if (idx < 0) idx = 0;
    if (idx > static_cast<int>(o.hi)) idx = static_cast<int>(o.hi);
    if (o.key[0] == 'l' && o.key[1] == 'a') {          // langue : nom natif
        std::snprintf(out, n, "%s", kLangNames[idx]);
    } else if (o.names[0] != S_COUNT) {
        std::snprintf(out, n, "%s", T(o.names[idx]));
    } else {
        std::snprintf(out, n, "%d%s", idx < 0 ? 0 : static_cast<int>(v + 0.5F), o.unit);
    }
}


// ---------------------------------------------------------------- ROM
enum RomState { kRomMissing, kRomOk, kRomBad };
RomState g_rom = kRomMissing;
char g_rom_path[80] = "";
char g_rom_info[64] = "";
const char* const kRomPaths[] = {"sd:/apps/DKR-R/dkr.z64", "sd:/apps/DKR-R/rom.z64", "sd:/roms/dkr.z64", "sd:/roms/n64/dkr.z64",
                                 "sd:/dkr.z64", "usb:/apps/DKR-R/dkr.z64", "usb:/roms/dkr.z64", "usb:/dkr.z64"};

// SHA-1 incremental (FIPS 180-4)
struct Sha1 {
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    unsigned char buf[64];
    size_t fill = 0;
    std::uint64_t total = 0;
    static std::uint32_t Rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void Block(const unsigned char* b) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(b[i * 4]) << 24) | (static_cast<std::uint32_t>(b[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(b[i * 4 + 2]) << 8) | b[i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999U; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1U; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCU; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6U; }
            const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(bb, 30); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    void Update(const unsigned char* p, size_t n) {
        total += n;
        while (n > 0) {
            if (fill == 0 && n >= 64) { Block(p); p += 64; n -= 64; continue; }
            const size_t take = n < 64 - fill ? n : 64 - fill;
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { Block(buf); fill = 0; }
        }
    }
    void Final(std::uint32_t out[5]) {
        const std::uint64_t bits = total * 8;
        const unsigned char pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (fill != 56) Update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        Update(len, 8);
        std::memcpy(out, h, sizeof(h));
    }
};

// Versions de la ROM prises en charge : SHA-1 apres normalisation de l'ordre des octets en .z64 (voir README)
const std::uint32_t kKnownRoms[2][5] = {
    {0x0cb115d8U, 0x716dbbc2U, 0x922fda38U, 0xe533b9feU, 0x63bb9670U},  // US v1.0
    {0x6d96743dU, 0x46f8c0cdU, 0x0edb0ec5U, 0x600b003cU, 0x89b93755U},  // US v1.1 (Rev A)
};

void DrawCheckScreen(int percent) {
    std::memcpy(g_frame, g_bg, static_cast<size_t>(g_w) * g_h * 4);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %d%%", T(S_ROM_CHECK), percent);
    FillRect(g_w / 2 - 190, g_h / 2 - 28, g_w / 2 + 190, g_h / 2 + 28, kNavy, 215);
    FrameRect(g_w / 2 - 190, g_h / 2 - 28, g_w / 2 + 190, g_h / 2 + 28, 2, kGold, 255);
    DrawTextCentered(kSmall, g_w / 2, g_h / 2 - kSmall.height / 2, buf, kWhite);
    Present();
}

// Cherche la ROM (memes emplacements que le chargeur du jeu) et verifie son SHA-1 : seules les deux versions connues sont acceptees.
void CheckRom() {
    g_rom = kRomMissing;
    std::snprintf(g_rom_path, sizeof(g_rom_path), "%s", kRomPaths[0]);
    for (const char* path : kRomPaths) {
        FILE* f = std::fopen(path, "rb");
        if (f == nullptr) continue;
        std::snprintf(g_rom_path, sizeof(g_rom_path), "%s", path);
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        g_rom = kRomBad;
        unsigned char head[4] = {};
        if (size == 12582912L && std::fread(head, 1, 4, f) == 4) {
            // ordre des octets : z64 80 37 12 40, v64 37 80 40 12 (paires echangees), n64 40 12 37 80 (mots inverses)
            const int order = (head[0] == 0x80 && head[1] == 0x37) ? 0 : (head[0] == 0x37 && head[1] == 0x80) ? 1 : (head[0] == 0x40 && head[1] == 0x12) ? 2 : -1;
            if (order >= 0) {
                std::fseek(f, 0, SEEK_SET);
                static unsigned char chunk[65536];
                Sha1 sha;
                long done = 0;
                int last_pct = -1;
                while (done < size) {
                    const size_t n = std::fread(chunk, 1, sizeof(chunk), f);
                    if (n == 0) break;
                    for (size_t i = 0; i + 3 < n; i += 4) {
                        unsigned char* q = chunk + i;
                        if (order == 1) { std::swap(q[0], q[1]); std::swap(q[2], q[3]); }
                        else if (order == 2) { std::swap(q[0], q[3]); std::swap(q[1], q[2]); }
                    }
                    sha.Update(chunk, n);
                    done += static_cast<long>(n);
                    const int pct = static_cast<int>(done * 100 / size);
                    if (pct != last_pct && (pct % 5) == 0) { last_pct = pct; DrawCheckScreen(pct); }
                }
                std::uint32_t d[5];
                sha.Final(d);
                for (int v = 0; v < 2; ++v) {
                    if (done == size && std::memcmp(d, kKnownRoms[v], sizeof(d)) == 0) {
                        g_rom = kRomOk;
                        std::snprintf(g_rom_info, sizeof(g_rom_info), "US v1.%d", v);
                    }
                }
            }
        }
        std::fclose(f);
        return;
    }
}

// Texte aligne a gauche sur plusieurs lignes (coupe aux espaces, ou n'importe ou pour le japonais) ; renvoie la hauteur utilisee.
int DrawWrapped(const Font& f, int x, int y, int max_w, const char* text, std::uint32_t color, bool centered) {
    std::string line;
    int lines = 0;
    const char* p = text;
    size_t last_space = std::string::npos;
    auto flush = [&](const std::string& l) {
        if (centered) DrawTextCentered(f, x + max_w / 2, y + lines * (f.height + 2), l.c_str(), color);
        else DrawText(f, x, y + lines * (f.height + 2), l.c_str(), color);
        ++lines;
    };
    while (*p) {
        const char* start = p;
        const int code = NextCode(p);
        line.append(start, p - start);
        if (code == ' ') last_space = line.size() - 1;
        if (TextWidth(f, line.c_str()) > max_w) {
            size_t cut = last_space != std::string::npos ? last_space : line.size() - static_cast<size_t>(p - start);
            std::string rest = line.substr(last_space != std::string::npos ? cut + 1 : cut);
            line.resize(cut);
            flush(line);
            line = rest;
            last_space = std::string::npos;
        }
    }
    if (!line.empty()) flush(line);
    return lines * (f.height + 2);
}

void DrawRomStatus() {
    if (g_rom == kRomOk) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s  (%s)", T(S_ROM_OK), g_rom_info);
        const int w = TextWidth(kSmall, buf) + 28;
        FillRect(g_w - w - 12, 10, g_w - 12, 10 + kSmall.height + 10, kNavy, 190);
        FrameRect(g_w - w - 12, 10, g_w - 12, 10 + kSmall.height + 10, 2, 0x5CD65C, 255);
        DrawText(kSmall, g_w - w + 2, 15, buf, 0x8CFF8C);
        return;
    }
    const bool missing = g_rom == kRomMissing;
    const int x0 = 60, x1 = g_w - 60, y0 = 70, y1 = y0 + 160;
    FillRect(x0, y0, x1, y1, kNavy, 225);
    FrameRect(x0, y0, x1, y1, 3, missing ? 0xFF5A4A : 0xFFA33A, 255);
    int y = y0 + 14;
    y += DrawWrapped(kSmall, x0 + 24, y, x1 - x0 - 48, T(missing ? S_ROM_MISSING : S_ROM_BAD), kWhite, true) + 10;
    DrawTextCentered(kSmall, g_w / 2, y, g_rom_path, kGold);
}

void DrawMain(int sel) {
    std::memcpy(g_frame, g_bg, static_cast<size_t>(g_w) * g_h * 4);
    const char* items[3] = {T(S_PLAY), T(S_OPTIONS), T(S_QUIT)};
    const int bw = 170, gap = 24, total = 3 * bw + 2 * gap;
    const int y = g_img_h + (g_h - g_img_h) / 2 - 36;
    for (int i = 0; i < 3; ++i) {
        const int x = (g_w - total) / 2 + i * (bw + gap);
        FillRect(x, y, x + bw, y + 44, kNavy, i == sel ? 200 : 120);
        FrameRect(x, y, x + bw, y + 44, i == sel ? 3 : 1, i == sel ? kGold : kGray, i == sel ? 255 : 120);
        DrawTextCentered(kBig, x + bw / 2, y + 4, items[i], i == sel ? kGold : kWhite);
    }
    DrawRomStatus();
    FillRect(0, g_h - 42, g_w, g_h, kNavy, 170);
    DrawTextCentered(kSmall, g_w / 2, g_h - 34, T(S_HINT_MAIN), kGray, 230);
}

void DrawOptions(int sel) {
    std::memcpy(g_frame, g_bg, static_cast<size_t>(g_w) * g_h * 4);
    FillRect(0, 0, g_w, g_h, 0x000000, 150);
    const int px0 = 30, px1 = g_w - 30, py0 = 22, py1 = g_h - 52;
    FillRect(px0, py0, px1, py1, kNavy, 215);
    FrameRect(px0, py0, px1, py1, 3, kGold, 255);
    DrawTextCentered(kBig, g_w / 2, py0 + 8, T(S_OPTIONS), kGold);
    const int rows = kOptionCount + 2; // + retablir + retour
    const int top = py0 + 56, rh = (py1 - top - 10) / rows;
    for (int i = 0; i < rows; ++i) {
        const int y = top + i * rh;
        const bool s = i == sel;
        if (s) {
            FillRect(px0 + 14, y, px1 - 14, y + rh - 3, kGold, 70);
            FrameRect(px0 + 14, y, px1 - 14, y + rh - 3, 2, kGold, 255);
        }
        const int ty = y + (rh - 3 - kSmall.height) / 2;
        if (i < kOptionCount) {
            char val[40];
            FormatValue(kOptions[i], GetCfg(kOptions[i].key, kOptions[i].def), val, sizeof(val));
            DrawText(kSmall, px0 + 30, ty, OptionLabel(kOptions[i]), s ? kWhite : kGray);
            char shown[64];
            std::snprintf(shown, sizeof(shown), s ? "<  %s  >" : "%s", val);
            DrawText(kSmall, px1 - 30 - TextWidth(kSmall, shown), ty, shown, s ? kGold : kWhite);
        } else {
            DrawTextCentered(kSmall, g_w / 2, ty, i == kOptionCount ? T(S_RESET) : T(S_BACK), s ? kGold : kWhite);
        }
    }
    FillRect(0, g_h - 46, g_w, g_h, kNavy, 170);
    DrawTextCentered(kSmall, g_w / 2, g_h - 38, T(S_HINT_OPTS), kGray, 230);
}

void ChangeOption(int i, int dir) {
    const Option& o = kOptions[i];
    float v = GetCfg(o.key, o.def) + dir * o.step;
    if (o.names[0] != S_COUNT || o.key[0] == 'l') { // enumeration (ou langue) : boucle
        if (v < o.lo) v = o.hi;
        if (v > o.hi) v = o.lo;
    } else {                     // valeur numerique : bornee
        v = v < o.lo ? o.lo : (v > o.hi ? o.hi : v);
    }
    SetCfg(o.key, v);
    g_lang = static_cast<int>(GetCfg("lang", 0.0F) + 0.5F);
}

} // namespace

// Texte superpose a l'image dans l'XFB (YUY2), utilise par l'overlay de diagnostic du moteur : fond assombri puis texte blanc.
extern "C" void dkr_wii_draw_overlay(void* xfb, int width, int x0, int y0, const char* text) {
    // Acces par mots de 32 bits (Y0 Cb Y1 Cr, grand-boutiste) sur l'XFB non cache
    std::uint32_t* base = static_cast<std::uint32_t*>(xfb);
    const Font& f = kSmall;
    const int w = (TextWidth(f, text) + 16 + 1) & ~1, h = f.height + 8;
    x0 &= ~1;
    auto row = [&](int y) { return base + static_cast<size_t>(y) * (width / 2); };
    for (int y = y0; y < y0 + h; ++y) { // fond : luminance reduite, chrominance neutre
        std::uint32_t* r = row(y) + x0 / 2;
        for (int k = 0; k < w / 2; ++k) {
            const int ya = static_cast<int>((r[k] >> 24) & 255), yb = static_cast<int>((r[k] >> 8) & 255);
            const std::uint32_t ya2 = static_cast<std::uint32_t>(16 + ((ya - 16) * 2) / 5), yb2 = static_cast<std::uint32_t>(16 + ((yb - 16) * 2) / 5);
            r[k] = (ya2 << 24) | (128U << 16) | (yb2 << 8) | 128U;
        }
    }
    int pen = x0 + 8;
    const char* p = text;
    while (*p) {
        const LauncherGlyph* g = FindGlyph(f, NextCode(p));
        if (g == nullptr) continue;
        for (int j = 0; j < g->h; ++j) {
            std::uint32_t* r = row(y0 + 4 + f.ascent + g->oy + j);
            for (int i = 0; i < g->w; ++i) {
                const int a = f.bits[g->off + j * g->w + i];
                if (a == 0) continue;
                const int px = pen + g->ox + i;
                std::uint32_t word = r[px / 2];
                const int shift = (px & 1) ? 8 : 24;
                const int Y = static_cast<int>((word >> shift) & 255);
                const std::uint32_t Yn = static_cast<std::uint32_t>(Y + ((235 - Y) * a) / 255);
                word = (word & ~(255U << shift)) | (Yn << shift);
                r[px / 2] = word;
            }
        }
        pen += g->adv;
    }
}

extern "C" bool dkr_wii_launcher_inited() { return g_inited; }

extern "C" void dkr_wii_launcher_run() {
    LoadCfgLines();
    if (GetCfg("launcher", 1.0F) == 0.0F) return;

    g_lang = static_cast<int>(GetCfg("lang", 0.0F) + 0.5F);
    if (g_lang < 0 || g_lang >= kLangCount) g_lang = 0;
    g_w = g_wii_rmode->fbWidth;
    g_h = g_wii_rmode->xfbHeight;
    g_frame = static_cast<std::uint32_t*>(std::malloc(static_cast<size_t>(g_w) * g_h * 4));
    BuildBackground();
    if (g_frame == nullptr || g_bg == nullptr) { // memoire insuffisante : on lance le jeu sans ecran
        std::free(g_frame);
        std::free(g_bg);
        g_frame = g_bg = nullptr;
        return;
    }
    CheckRom();
    PAD_Init();
    WPAD_Init();
    g_inited = true;

    enum { kMain, kOpts } screen = GetCfg("launcher", 1.0F) == 2.0F ? kOpts : kMain; // launcher=2 : demarre sur les options (verification visuelle)
    int sel_main = 0, sel_opt = 0;
    bool dirty = true;
    bool go = false;
    int settle = 20; // laisse les manettes se declarer avant de lire les entrees
    while (!go) {
        VIDEO_WaitVSync();
        const Input in = PollInput();
        if (settle > 0) { --settle; if (dirty) { if (screen == kOpts) DrawOptions(sel_opt); else DrawMain(sel_main); Present(); dirty = false; } continue; }
        if (screen == kMain) {
            if (in.left && sel_main > 0) { --sel_main; dirty = true; }
            if (in.right && sel_main < 2) { ++sel_main; dirty = true; }
            if (in.start && g_rom == kRomOk) go = true;
            if (in.ok) {
                if (sel_main == 0) { if (g_rom == kRomOk) go = true; else { CheckRom(); dirty = true; } }
                else if (sel_main == 1) { screen = kOpts; sel_opt = 0; dirty = true; }
                else std::exit(0);
            }
            if (dirty && !go) { DrawMain(sel_main); Present(); dirty = false; }
        } else {
            const int rows = kOptionCount + 2;
            if (in.up) { sel_opt = (sel_opt + rows - 1) % rows; dirty = true; }
            if (in.down) { sel_opt = (sel_opt + 1) % rows; dirty = true; }
            if (sel_opt < kOptionCount && (in.left || in.right)) { ChangeOption(sel_opt, in.right ? 1 : -1); dirty = true; }
            if (in.ok) {
                if (sel_opt < kOptionCount) { ChangeOption(sel_opt, 1); dirty = true; }
                else if (sel_opt == kOptionCount) { for (const Option& o : kOptions) SetCfg(o.key, o.def); g_lang = 0; dirty = true; }
                else { SaveCfgLines(); screen = kMain; dirty = true; }
            }
            if (in.back) { SaveCfgLines(); screen = kMain; dirty = true; }
            if (in.start) { SaveCfgLines(); go = true; }
            if (dirty && !go) {
                if (screen == kOpts) DrawOptions(sel_opt); else DrawMain(sel_main);
                Present();
                dirty = false;
            }
        }
    }
    SaveCfgLines();
    std::free(g_frame);
    std::free(g_bg);
    g_frame = g_bg = nullptr;
    VIDEO_ClearFrameBuffer(g_wii_rmode, g_wii_xfb, COLOR_BLACK);
}

#endif // GEKKO
