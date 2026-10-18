// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
// Journal de debug par reseau (UDP) pour la Wii : envoie des lignes de texte vers un PC (tools/wii/netlog_listener.py).
// Active par `net=1` dans sd:/apps/DKR-R/dkr-wii.cfg ; hote : adresse IPv4 du PC dans le fichier
// sd:/apps/DKR-R/loghost.txt (une adresse IPv4). Port UDP 4300.
// Tout se passe dans un thread dedie (net_init bloque jusqu'au DHCP) : le jeu n'est jamais retarde.
#if defined(GEKKO)
#include <gccore.h>
#include <network.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

extern "C" float dkr_wii_cfg(const char* key, float def);
extern "C" volatile unsigned g_hb_present;
extern "C" volatile unsigned g_hb_dl;
extern "C" volatile int g_hb_menu;
extern "C" int dkr_live_set(const char* key, float v);
extern "C" int dkr_live_get(const char* key, float* out);
extern "C" void dkr_live_list(char* out, int cap);
extern "C" const char* dkr_live_last_prof();
extern "C" void dkr_shot_request(int color);
extern "C" void dkr_live_dbg(char* out, int cap);

namespace {
constexpr unsigned kQueue = 32;
constexpr unsigned kLine = 240;
char g_queue[kQueue][kLine];
volatile unsigned g_qr = 0, g_qw = 0;
volatile bool g_ready = false;
volatile bool g_stop = false, g_done = false;
lwp_t g_thread = 0;
bool g_started = false;
const u8* g_shot_buf = nullptr;
volatile u16 g_shot_w = 0, g_shot_h = 0;
volatile bool g_shot_busy = false;
volatile int g_shot_color = 0;

void Enqueue(const char* line) {
    const unsigned w = g_qw;
    if (w - g_qr >= kQueue) return; // file pleine : on perd la ligne
    std::strncpy(g_queue[w % kQueue], line, kLine - 1);
    g_queue[w % kQueue][kLine - 1] = 0;
    g_qw = w + 1;
}

void SendShot(s32 sock, struct sockaddr_in* addr) {
    // Paquet : 'Z' 'S', largeur, hauteur, indice, total (u16 grand-boutiste), puis jusqu'a 1000 octets de Z (3 octets par pixel).
    const u32 total_bytes = static_cast<u32>(g_shot_w) * g_shot_h * 3U;
    const u16 total = static_cast<u16>((total_bytes + 999U) / 1000U);
    u8 pkt[1010];
    for (u16 i = 0; i < total; ++i) {
        const u32 off = static_cast<u32>(i) * 1000U;
        const u32 n = total_bytes - off < 1000U ? total_bytes - off : 1000U;
        pkt[0] = 'Z'; pkt[1] = g_shot_color ? 'C' : 'S';
        pkt[2] = static_cast<u8>(g_shot_w >> 8); pkt[3] = static_cast<u8>(g_shot_w);
        pkt[4] = static_cast<u8>(g_shot_h >> 8); pkt[5] = static_cast<u8>(g_shot_h);
        pkt[6] = static_cast<u8>(i >> 8); pkt[7] = static_cast<u8>(i);
        pkt[8] = static_cast<u8>(total >> 8); pkt[9] = static_cast<u8>(total);
        std::memcpy(pkt + 10, g_shot_buf + off, n);
        net_sendto(sock, pkt, static_cast<s32>(n + 10U), 0, reinterpret_cast<struct sockaddr*>(addr), 8);
        usleep(4000); // cadence douce : le Wi-Fi de la Wii perd les rafales
    }
    g_shot_busy = false;
}

void* NetThread(void*) {
    char host[32] = ""; // pas d'adresse par defaut : le journal reseau demarre seulement si sd:/apps/DKR-R/loghost.txt existe
    if (FILE* f = std::fopen("sd:/apps/DKR-R/loghost.txt", "r")) {
        char tmp[32] = {};
        if (std::fscanf(f, "%31s", tmp) == 1) std::strncpy(host, tmp, sizeof(host) - 1);
        std::fclose(f);
    }
    if (host[0] == 0) return nullptr; // sans loghost.txt : aucun reseau, aucun envoi
    s32 r = -1;
    for (int attempt = 1; attempt <= 6; ++attempt) { // plusieurs essais : le lien / DHCP peut etre lent
        r = net_init();
        if (FILE* f = std::fopen("sd:/apps/DKR-R/netlog-status.txt", attempt == 1 ? "w" : "a")) {
            std::fprintf(f, "essai %d : net_init = %d\n", attempt, static_cast<int>(r));
            std::fclose(f);
        }
        if (r >= 0) break;
        usleep(3000000);
    }
    if (r < 0) return nullptr;
    const s32 sock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return nullptr;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_len = 8;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4300);
    inet_aton(host, &addr.sin_addr);
    g_ready = true;
    // Canal de commandes (UDP 4301), sans blocage ; seules les commandes venant de l'hote du journal sont acceptees.
    s32 csock = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (csock >= 0) {
        struct sockaddr_in me;
        std::memset(&me, 0, sizeof(me));
        me.sin_len = 8;
        me.sin_family = AF_INET;
        me.sin_port = htons(4301);
        me.sin_addr.s_addr = INADDR_ANY;
        u32 one = 1;
        if (net_bind(csock, reinterpret_cast<struct sockaddr*>(&me), 8) < 0 || net_ioctl(csock, FIONBIO, &one) < 0) {
            net_close(csock);
            csock = -1;
        }
    }
    char hello[120];
    struct in_addr own;
    own.s_addr = net_gethostip();
    std::snprintf(hello, sizeof(hello), "NETLOG start host=%s ip=%s", host, inet_ntoa(own));
    net_sendto(sock, hello, static_cast<s32>(std::strlen(hello)), 0, reinterpret_cast<struct sockaddr*>(&addr), 8);
    unsigned beat = 0;
    while (!g_stop) {
        while (g_qr != g_qw) {
            const char* line = g_queue[g_qr % kQueue];
            net_sendto(sock, line, static_cast<s32>(std::strlen(line)), 0, reinterpret_cast<struct sockaddr*>(&addr), 8);
            g_qr = g_qr + 1;
        }
        usleep(50000);
        while (csock >= 0) {
            char cmd[96] = {};
            struct sockaddr_in from;
            u32 flen = 8;
            const s32 n = net_recvfrom(csock, cmd, sizeof(cmd) - 1, 0, reinterpret_cast<struct sockaddr*>(&from), &flen);
            if (n <= 0) break;
            if (from.sin_addr.s_addr != addr.sin_addr.s_addr) continue; // pas l'hote du journal
            for (s32 k = 0; k < n; ++k) if (cmd[k] == 13 || cmd[k] == 10) cmd[k] = 0;
            char key[40] = {};
            float val = 0.0F;
            char rep[400];
            if (std::sscanf(cmd, "set %39s %f", key, &val) == 2) {
                std::snprintf(rep, sizeof(rep), dkr_live_set(key, val) ? "OK %s=%g" : "ERR inconnu: %s", key, static_cast<double>(val));
            } else if (std::sscanf(cmd, "get %39s", key) == 1) {
                if (dkr_live_get(key, &val)) std::snprintf(rep, sizeof(rep), "OK %s=%g", key, static_cast<double>(val));
                else std::snprintf(rep, sizeof(rep), "ERR inconnu: %s", key);
            } else if (std::strncmp(cmd, "prof", 4) == 0) {
                std::snprintf(rep, sizeof(rep), "PROF %s", dkr_live_last_prof());
            } else if (std::strncmp(cmd, "list", 4) == 0) {
                char names[300];
                dkr_live_list(names, sizeof(names));
                std::snprintf(rep, sizeof(rep), "VARS %s", names);
            } else if (std::strncmp(cmd, "shot", 4) == 0) {
                if (g_shot_busy) { std::snprintf(rep, sizeof(rep), "ERR shot en cours d'envoi"); }
                else { dkr_shot_request(cmd[4] == 'c' ? 1 : 0); std::snprintf(rep, sizeof(rep), "OK %s demande (quelques secondes)", cmd[4] == 'c' ? "shotc (couleur 240x240)" : "shot (profondeur 120x120)"); }
            } else if (std::strncmp(cmd, "dbg", 3) == 0) {
                char d[300];
                dkr_live_dbg(d, sizeof(d));
                std::snprintf(rep, sizeof(rep), "DBG %s", d);
            } else if (std::strncmp(cmd, "ping", 4) == 0) {
                std::snprintf(rep, sizeof(rep), "PONG present=%u dl=%u", g_hb_present, g_hb_dl);
            } else {
                std::snprintf(rep, sizeof(rep), "ERR commandes: set K V | get K | prof | list | shot | ping");
            }
            net_sendto(sock, rep, static_cast<s32>(std::strlen(rep)), 0, reinterpret_cast<struct sockaddr*>(&addr), 8);
        }
        if (g_shot_busy) SendShot(sock, &addr);
        if ((++beat % 20U) == 0U) { // battement de coeur chaque seconde : presents, display lists, ecran du jeu
            char hb[100];
            std::snprintf(hb, sizeof(hb), "HB present=%u dl=%u menu=%d", g_hb_present, g_hb_dl, static_cast<int>(g_hb_menu));
            net_sendto(sock, hb, static_cast<s32>(std::strlen(hb)), 0, reinterpret_cast<struct sockaddr*>(&addr), 8);
        }
    }
    // Sortie propre : sans net_deinit() le module reseau de l'IOS reste dans un etat qui coupe le Wi-Fi au retour au Homebrew Channel.
    if (csock >= 0) net_close(csock);
    net_close(sock);
    net_deinit();
    g_done = true;
    return nullptr;
}
} // namespace

extern "C" int dkr_shot_submit(const u8* z24, u16 w, u16 h, int color) {
    if (g_shot_busy) return 0;
    g_shot_color = color;
    g_shot_buf = z24; g_shot_w = w; g_shot_h = h;
    g_shot_busy = true;
    return 1;
}

extern "C" void dkr_netlog(const char* line) {
    if (g_started) Enqueue(line);
}

extern "C" void dkr_netlog_stop() {
    if (!g_started || !g_ready) return;
    g_stop = true;
    for (int i = 0; i < 40 && !g_done; ++i) usleep(50000);
}

extern "C" void dkr_netlog_start() {
    if (g_started) return;
    if (dkr_wii_cfg("diag", 0.0F) == 0.0F) return; // panneau de debug sur Arret : aucun thread, aucun socket, aucune memoire reseau
    if (dkr_wii_cfg("net", 1.0F) == 0.0F) return;  // net=0 : coupe explicite meme avec le panneau de debug
    if (FILE* lf = std::fopen("sd:/apps/DKR-R/loghost.txt", "r")) std::fclose(lf); else return; // sans loghost.txt : pas de reseau
    g_started = true;
    static u8 stack[32768] __attribute__((aligned(32)));
    LWP_CreateThread(&g_thread, NetThread, nullptr, stack, sizeof(stack), 64);
}
#endif
