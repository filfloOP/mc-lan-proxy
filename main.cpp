// mc_lan_proxy - plugin GoldHEN (PRX) pour Minecraft Bedrock PS4 (LAN games)
// Lit /data/mc_lan_proxy/servers.ini, annonce chaque serveur dans l'onglet
// "LAN games" et relaie le trafic UDP vers le vrai serveur distant.
//
// IMPORTANT : ce code n'utilise PAS la libc (musl) liee statiquement. Dans un
// plugin charge dans le jeu, elle plante (TLS non initialise). On utilise
// uniquement les fonctions systeme Sony (libkernel / libSceNet).

#include <stdint.h>
#include <stddef.h>

typedef void* ScePthread;

extern "C" {
int     sceKernelOpen(const char* path, int flags, unsigned mode);
int64_t sceKernelRead(int fd, void* buf, size_t n);
int64_t sceKernelWrite(int fd, const void* buf, size_t n);
int     sceKernelClose(int fd);
int     sceKernelMkdir(const char* path, unsigned mode);
int     sceKernelUsleep(unsigned usec);
int     scePthreadCreate(ScePthread* t, const void* attr, void* (*entry)(void*), void* arg, const char* name);
int     scePthreadJoin(ScePthread t, void** ret);
int     sceKernelSendNotificationRequest(int device, void* req, size_t size, int blocking);

int sceNetSocket(const char* name, int family, int type, int protocol);
int sceNetBind(int s, const void* addr, unsigned addrlen);
int sceNetSendto(int s, const void* buf, size_t len, int flags, const void* to, unsigned tolen);
int sceNetRecvfrom(int s, void* buf, size_t len, int flags, void* from, unsigned* fromlen);
int sceNetSetsockopt(int s, int level, int optname, const void* optval, unsigned optlen);
int sceNetSocketClose(int s);
int sceNetInetPton(int af, const char* src, void* dst);
}

// ---------------------------------------------------------------- config
#define CONFIG_DIR   "/data/mc_lan_proxy"
#define CONFIG_PATH  "/data/mc_lan_proxy/servers.ini"
#define LOG_PATH     "/data/mc_lan_proxy/log.txt"
#define LISTEN_PORT  19132
#define MAX_SERVERS  10
#define BUF_SIZE     2048

#define SCE_AF_INET        2
#define SCE_SOCK_DGRAM     2
#define SCE_SOL_SOCKET     0xffff
#define SCE_SO_REUSEADDR   0x0004
#define SCE_SO_NBIO        0x1200

struct SockAddrIn {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;      // ordre reseau
    uint32_t addr;      // ordre reseau
    uint16_t vport;
    char     zero[6];
};

struct NotifyRequest {
    char pad[45];
    char message[3075];
};

struct Server {
    char name[64];
    char ip[64];
    int  port;
    int  protocol;      // 0 = valeur globale
    char version[32];   // "" = valeur globale
    int  sock;
    int  local_port;
    SockAddrIn remote;
    SockAddrIn client;
    bool has_client;
};

static Server g_servers[MAX_SERVERS];
static int  g_count = 0;
static int  g_protocol = 589;
static char g_version[32] = "1.20.10";
static int  g_relay_base = 19140;

static ScePthread g_thread;
static volatile bool g_running = false;
static int g_listen = -1;
static unsigned char g_buf[BUF_SIZE];

static const unsigned char RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78
};

// ---------------------------------------------------------------- utilitaires (sans libc)
static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static void zero(void* p, size_t n) {
    volatile char* c = (volatile char*)p;
    for (size_t i = 0; i < n; i++) c[i] = 0;
}

static void scopy(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static bool streq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
        a++; b++;
    }
    return *a == *b;
}

static int to_int(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void trim(char* s) {
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') start++;
    size_t n = slen(s);
    size_t end = n;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r' || s[end-1] == '\n')) end--;
    size_t k = 0;
    for (size_t i = start; i < end; i++) s[k++] = s[i];
    s[k] = 0;
}

static uint16_t hton16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// petit constructeur de chaine
struct Str { char d[256]; int n; };
static void sinit(Str& s) { s.n = 0; s.d[0] = 0; }
static void sa(Str& s, const char* t) { while (*t && s.n < 255) s.d[s.n++] = *t++; s.d[s.n] = 0; }
static void si(Str& s, int v) {
    char tmp[12]; int k = 0;
    if (v < 0) { sa(s, "-"); v = -v; }
    if (v == 0) tmp[k++] = '0';
    while (v > 0) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    char o[12]; int j = 0;
    while (k > 0) o[j++] = tmp[--k];
    o[j] = 0;
    sa(s, o);
}

static void log_line(const char* text) {
    int fd = sceKernelOpen(LOG_PATH, 0x209 /* WRONLY|CREAT|APPEND */, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, text, slen(text));
    sceKernelWrite(fd, "\n", 1);
    sceKernelClose(fd);
}

static void notify(const char* msg) {
    static NotifyRequest r;
    zero(&r, sizeof(r));
    scopy(r.message, sizeof(r.message), msg);
    sceKernelSendNotificationRequest(0, &r, sizeof(r), 0);
}

// ---------------------------------------------------------------- ini
static const char* DEFAULT_INI =
    "# mc_lan_proxy - liste de serveurs\n"
    "# Chaque [section] = un serveur affiche dans Jouer > Amis > LAN games\n"
    "# Limite : 10 serveurs. IP au format 1.2.3.4 (pas de nom de domaine).\n"
    "\n"
    "[settings]\n"
    "protocol=589\n"
    "version=1.20.10\n"
    "relay_base_port=19140\n"
    "\n"
    "[Exemple]\n"
    "name=Mon Serveur\n"
    "ip=1.2.3.4\n"
    "port=19132\n";

static void write_default_ini() {
    sceKernelMkdir(CONFIG_DIR, 0777);
    int fd = sceKernelOpen(CONFIG_PATH, 0x601 /* WRONLY|CREAT|TRUNC */, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, DEFAULT_INI, slen(DEFAULT_INI));
    sceKernelClose(fd);
}

static void commit_server(Server& cur, bool& have) {
    bool fake = (cur.ip[0] == '1' && cur.ip[1] == '.' && cur.ip[2] == '2' && cur.ip[3] == '.' &&
                 cur.ip[4] == '3' && cur.ip[5] == '.' && cur.ip[6] == '4' && cur.ip[7] == 0);
    if (have && cur.ip[0] && !fake && g_count < MAX_SERVERS) {
        if (!cur.name[0]) scopy(cur.name, sizeof(cur.name), "Serveur");
        g_servers[g_count++] = cur;
    }
    zero(&cur, sizeof(cur));
    cur.port = 19132;
    cur.sock = -1;
    have = false;
}

static void handle_line(char* line, Server& cur, bool& have, bool& in_settings) {
    trim(line);
    if (line[0] == 0 || line[0] == '#' || line[0] == ';') return;

    if (line[0] == '[') {
        commit_server(cur, have);
        char* title = line + 1;
        for (char* p = title; *p; ++p) if (*p == ']') { *p = 0; break; }
        trim(title);
        in_settings = streq_ci(title, "settings");
        if (!in_settings) {
            scopy(cur.name, sizeof(cur.name), title);
            have = true;
        }
        return;
    }

    char* eq = line;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') return;
    *eq = 0;
    char* key = line;
    char* val = eq + 1;
    trim(key);
    trim(val);

    if (in_settings) {
        if (streq_ci(key, "protocol")) g_protocol = to_int(val);
        else if (streq_ci(key, "version")) scopy(g_version, sizeof(g_version), val);
        else if (streq_ci(key, "relay_base_port")) g_relay_base = to_int(val);
    } else if (have) {
        if (streq_ci(key, "name")) scopy(cur.name, sizeof(cur.name), val);
        else if (streq_ci(key, "ip")) scopy(cur.ip, sizeof(cur.ip), val);
        else if (streq_ci(key, "port")) cur.port = to_int(val);
        else if (streq_ci(key, "protocol")) cur.protocol = to_int(val);
        else if (streq_ci(key, "version")) scopy(cur.version, sizeof(cur.version), val);
    }
}

static void load_ini() {
    g_count = 0;
    static char filebuf[8192];
    int fd = sceKernelOpen(CONFIG_PATH, 0 /* RDONLY */, 0);
    if (fd < 0) {
        write_default_ini();
        log_line("servers.ini absent: fichier d'exemple cree");
        return;
    }
    int64_t total = sceKernelRead(fd, filebuf, sizeof(filebuf) - 1);
    sceKernelClose(fd);
    if (total <= 0) return;
    filebuf[total] = 0;

    Server cur;
    zero(&cur, sizeof(cur));
    cur.port = 19132;
    cur.sock = -1;
    bool have = false, in_settings = false;

    char line[256];
    int ln = 0;
    for (int64_t i = 0; i <= total; i++) {
        char c = (i < total) ? filebuf[i] : '\n';
        if (c == '\n') {
            line[ln] = 0;
            handle_line(line, cur, have, in_settings);
            ln = 0;
        } else if (ln < (int)sizeof(line) - 1) {
            line[ln++] = c;
        }
    }
    commit_server(cur, have);
}

// ---------------------------------------------------------------- reseau
static int open_udp(int port) {
    int s = sceNetSocket("mclan", SCE_AF_INET, SCE_SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    sceNetSetsockopt(s, SCE_SOL_SOCKET, SCE_SO_REUSEADDR, &one, sizeof(one));
    sceNetSetsockopt(s, SCE_SOL_SOCKET, SCE_SO_NBIO, &one, sizeof(one));
    SockAddrIn a;
    zero(&a, sizeof(a));
    a.len = sizeof(a);
    a.family = SCE_AF_INET;
    a.port = hton16((uint16_t)port);
    a.addr = 0;
    if (sceNetBind(s, &a, sizeof(a)) < 0) {
        sceNetSocketClose(s);
        return -1;
    }
    return s;
}

static void send_pong(int idx, const SockAddrIn* to, const unsigned char* ping) {
    Server& s = g_servers[idx];
    if (s.sock < 0) return;

    char name[64];
    scopy(name, sizeof(name), s.name);
    for (char* p = name; *p; ++p) if (*p == ';') *p = ',';

    uint64_t guid = 0x1122334455667788ULL + (uint64_t)idx;

    // MCPE;nom;protocole;version;joueurs;max;guid;sousnom;mode;modeNum;port4;port6;
    char payload[300];
    int plen = 0;
    {
        Str t; sinit(t);
        sa(t, "MCPE;"); sa(t, name); sa(t, ";");
        si(t, s.protocol ? s.protocol : g_protocol); sa(t, ";");
        sa(t, s.version[0] ? s.version : g_version);
        sa(t, ";0;20;");
        si(t, (int)(guid & 0x7fffffff)); sa(t, ";mc_lan_proxy;Survival;1;");
        si(t, s.local_port); sa(t, ";"); si(t, s.local_port); sa(t, ";");
        for (int i = 0; i < t.n && i < (int)sizeof(payload); i++) payload[i] = t.d[i];
        plen = t.n;
    }

    unsigned char out[400];
    int pos = 0;
    out[pos++] = 0x1c;
    for (int i = 0; i < 8; i++) out[pos++] = ping[1 + i];             // temps du ping
    for (int i = 0; i < 8; i++) out[pos++] = (unsigned char)((guid >> (8 * i)) & 0xff);
    for (int i = 0; i < 16; i++) out[pos++] = RAKNET_MAGIC[i];
    out[pos++] = (unsigned char)((plen >> 8) & 0xff);
    out[pos++] = (unsigned char)(plen & 0xff);
    for (int i = 0; i < plen; i++) out[pos++] = (unsigned char)payload[i];

    // Envoye DEPUIS le socket relais : le jeu se connecte a ip_PS4:local_port
    sceNetSendto(s.sock, out, (size_t)pos, 0, to, sizeof(*to));
}

static bool same_addr(const SockAddrIn& a, const SockAddrIn& b) {
    return a.addr == b.addr && a.port == b.port;
}

static int setup_sockets() {
    int ok = 0;
    for (int i = 0; i < g_count; i++) {
        Server& s = g_servers[i];
        s.sock = -1;
        s.has_client = false;
        s.local_port = g_relay_base + i;

        zero(&s.remote, sizeof(s.remote));
        s.remote.len = sizeof(s.remote);
        s.remote.family = SCE_AF_INET;
        s.remote.port = hton16((uint16_t)s.port);
        if (sceNetInetPton(SCE_AF_INET, s.ip, &s.remote.addr) != 1) {
            Str t; sinit(t); sa(t, "["); sa(t, s.name); sa(t, "] IP invalide: "); sa(t, s.ip);
            log_line(t.d);
            continue;
        }
        s.sock = open_udp(s.local_port);
        if (s.sock < 0) {
            Str t; sinit(t); sa(t, "["); sa(t, s.name); sa(t, "] bind impossible, port local "); si(t, s.local_port);
            log_line(t.d);
            continue;
        }
        Str t; sinit(t); sa(t, "["); sa(t, s.name); sa(t, "] "); sa(t, s.ip); sa(t, ":"); si(t, s.port);
        sa(t, " via port local "); si(t, s.local_port);
        log_line(t.d);
        ok++;
    }
    return ok;
}

static void* proxy_thread(void*) {
    g_listen = open_udp(LISTEN_PORT);
    if (g_listen < 0) {
        log_line("bind impossible sur le port 19132");
        notify("mc_lan_proxy: port 19132 indisponible");
    }

    while (g_running) {
        bool idle = true;

        // 1) pings de decouverte LAN
        if (g_listen >= 0) {
            SockAddrIn from;
            unsigned fl = sizeof(from);
            zero(&from, sizeof(from));
            int n = sceNetRecvfrom(g_listen, g_buf, BUF_SIZE, 0, &from, &fl);
            if (n > 0) {
                idle = false;
                if (n >= 25 && (g_buf[0] == 0x01 || g_buf[0] == 0x02)) {
                    for (int i = 0; i < g_count; i++) send_pong(i, &from, g_buf);
                }
            }
        }

        // 2) relais joueur <-> serveur
        for (int i = 0; i < g_count; i++) {
            Server& s = g_servers[i];
            if (s.sock < 0) continue;
            SockAddrIn from;
            unsigned fl = sizeof(from);
            zero(&from, sizeof(from));
            int n = sceNetRecvfrom(s.sock, g_buf, BUF_SIZE, 0, &from, &fl);
            if (n <= 0) continue;
            idle = false;

            if (same_addr(from, s.remote)) {
                if (s.has_client) sceNetSendto(s.sock, g_buf, (size_t)n, 0, &s.client, sizeof(s.client));
            } else {
                s.client = from;
                s.has_client = true;
                sceNetSendto(s.sock, g_buf, (size_t)n, 0, &s.remote, sizeof(s.remote));
            }
        }

        if (idle) sceKernelUsleep(2000);
    }

    for (int i = 0; i < g_count; i++) {
        if (g_servers[i].sock >= 0) { sceNetSocketClose(g_servers[i].sock); g_servers[i].sock = -1; }
    }
    if (g_listen >= 0) { sceNetSocketClose(g_listen); g_listen = -1; }
    return 0;
}

// ---------------------------------------------------------------- entree
static void start_proxy() {
    if (g_running) return;

    notify("mc_lan_proxy: demarrage...");

    sceKernelMkdir(CONFIG_DIR, 0777);
    log_line("---- start ----");
    load_ini();
    int ok = setup_sockets();

    Str m; sinit(m);
    if (g_count == 0) {
        sa(m, "mc_lan_proxy charge - aucun serveur dans servers.ini");
    } else {
        sa(m, "mc_lan_proxy charge - "); si(m, ok); sa(m, "/"); si(m, g_count); sa(m, " serveur(s) actif(s)");
    }
    notify(m.d);

    g_running = true;
    if (scePthreadCreate(&g_thread, 0, proxy_thread, 0, "mc_lan_proxy") != 0) {
        g_running = false;
        log_line("scePthreadCreate a echoue");
        notify("mc_lan_proxy: erreur thread");
    }
}

static void stop_proxy() {
    if (g_running) {
        g_running = false;
        scePthreadJoin(g_thread, 0);
    }
}

// Points d'entree GoldHEN
extern "C" __attribute__((visibility("default"))) int plugin_load(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    start_proxy();
    return 0;
}

extern "C" __attribute__((visibility("default"))) int plugin_unload(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    stop_proxy();
    return 0;
}

// Points d'entree PRX classiques : ne font rien (le demarrage passe par plugin_load)
extern "C" __attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args; (void)argp;
    return 0;
}

extern "C" __attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args; (void)argp;
    return 0;
}
