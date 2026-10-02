// mc_lan_proxy - PRX PS4 pour Minecraft Bedrock (LAN games)
// Lit /data/mc_lan_proxy/servers.ini, annonce chaque serveur dans l'onglet
// "LAN games" et relaie le trafic UDP vers le vrai serveur distant.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ---------------------------------------------------------------- config
#define CONFIG_DIR   "/data/mc_lan_proxy"
#define CONFIG_PATH  "/data/mc_lan_proxy/servers.ini"
#define LOG_PATH     "/data/mc_lan_proxy/log.txt"
#define LISTEN_PORT  19132
#define MAX_SERVERS  10
#define BUF_SIZE     4096

// Notification PS4 (libkernel)
struct NotifyRequest {
    char pad[45];
    char message[3075];
};
extern "C" int sceKernelSendNotificationRequest(int device, void* req, size_t size, int blocking);

struct Server {
    char name[64];
    char ip[64];
    int  port;
    int  protocol;      // 0 = valeur globale
    char version[32];   // "" = valeur globale
    // runtime
    int  sock;
    int  local_port;
    sockaddr_in remote;
    sockaddr_in client;
    bool has_client;
};

static Server g_servers[MAX_SERVERS];
static int  g_count = 0;
static int  g_protocol = 589;
static char g_version[32] = "1.20.10";
static int  g_relay_base = 19140;

static pthread_t g_thread;
static volatile bool g_running = false;
static int g_listen = -1;

static const unsigned char RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78
};

// ---------------------------------------------------------------- utils
static void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void log_line(const char* fmt, ...) {
    FILE* f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void notify(const char* msg) {
    NotifyRequest r;
    char* p = (char*)&r;
    for (size_t i = 0; i < sizeof(r); i++) p[i] = 0;   // pas de libc ici
    size_t i = 0;
    while (msg[i] && i < sizeof(r.message) - 1) { r.message[i] = msg[i]; i++; }
    sceKernelSendNotificationRequest(0, &r, sizeof(r), 0);
}

static void trim(char* s) {
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void copy_str(char* dst, size_t cap, const char* src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
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
    mkdir(CONFIG_DIR, 0777);
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fputs(DEFAULT_INI, f);
    fclose(f);
}

static void commit_server(Server& cur, bool& have) {
    if (have && cur.ip[0] && strcmp(cur.ip, "1.2.3.4") != 0 && g_count < MAX_SERVERS) {
        if (!cur.name[0]) copy_str(cur.name, sizeof(cur.name), "Serveur");
        g_servers[g_count++] = cur;
    }
    memset(&cur, 0, sizeof(cur));
    cur.port = 19132;
    cur.sock = -1;
    have = false;
}

static void load_ini() {
    g_count = 0;
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        write_default_ini();
        log_line("servers.ini absent: fichier d'exemple cree");
        return;
    }

    Server cur;
    memset(&cur, 0, sizeof(cur));
    cur.port = 19132;
    cur.sock = -1;
    bool have = false;
    bool in_settings = false;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == 0 || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            commit_server(cur, have);
            char* end = strchr(line, ']');
            if (end) *end = 0;
            char* title = line + 1;
            trim(title);
            in_settings = (strcasecmp(title, "settings") == 0);
            if (!in_settings) {
                copy_str(cur.name, sizeof(cur.name), title);
                have = true;
            }
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        trim(key);
        trim(val);

        if (in_settings) {
            if (strcasecmp(key, "protocol") == 0) g_protocol = atoi(val);
            else if (strcasecmp(key, "version") == 0) copy_str(g_version, sizeof(g_version), val);
            else if (strcasecmp(key, "relay_base_port") == 0) g_relay_base = atoi(val);
        } else if (have) {
            if (strcasecmp(key, "name") == 0) copy_str(cur.name, sizeof(cur.name), val);
            else if (strcasecmp(key, "ip") == 0) copy_str(cur.ip, sizeof(cur.ip), val);
            else if (strcasecmp(key, "port") == 0) cur.port = atoi(val);
            else if (strcasecmp(key, "protocol") == 0) cur.protocol = atoi(val);
            else if (strcasecmp(key, "version") == 0) copy_str(cur.version, sizeof(cur.version), val);
        }
    }
    commit_server(cur, have);
    fclose(f);
}

// ---------------------------------------------------------------- reseau
static int open_udp(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void send_pong(int idx, const sockaddr_in* to, const unsigned char* ping) {
    Server& s = g_servers[idx];
    if (s.sock < 0) return;

    char name[64];
    copy_str(name, sizeof(name), s.name);
    for (char* p = name; *p; ++p) if (*p == ';') *p = ',';

    uint64_t guid = 0x1122334455667788ULL + (uint64_t)idx;
    char payload[400];
    int plen = snprintf(payload, sizeof(payload),
        "MCPE;%s;%d;%s;0;20;%llu;mc_lan_proxy;Survival;1;%d;%d;",
        name,
        s.protocol ? s.protocol : g_protocol,
        s.version[0] ? s.version : g_version,
        (unsigned long long)guid,
        s.local_port, s.local_port);
    if (plen <= 0) return;

    unsigned char out[512];
    int pos = 0;
    out[pos++] = 0x1c;
    memcpy(out + pos, ping + 1, 8);  pos += 8;     // temps du ping
    memcpy(out + pos, &guid, 8);     pos += 8;
    memcpy(out + pos, RAKNET_MAGIC, 16); pos += 16;
    out[pos++] = (unsigned char)((plen >> 8) & 0xff);
    out[pos++] = (unsigned char)(plen & 0xff);
    memcpy(out + pos, payload, (size_t)plen); pos += plen;

    // Envoi DEPUIS le socket relais : le jeu se connectera a ip_PS4:local_port
    sendto(s.sock, out, (size_t)pos, 0, (const sockaddr*)to, sizeof(*to));
}

static bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static int setup_sockets() {
    int ok = 0;
    for (int i = 0; i < g_count; i++) {
        Server& s = g_servers[i];
        s.sock = -1;
        s.has_client = false;
        s.local_port = g_relay_base + i;

        memset(&s.remote, 0, sizeof(s.remote));
        s.remote.sin_family = AF_INET;
        s.remote.sin_port = htons((uint16_t)s.port);
        if (inet_pton(AF_INET, s.ip, &s.remote.sin_addr) != 1) {
            log_line("[%s] IP invalide: %s (noms de domaine non supportes)", s.name, s.ip);
            continue;
        }
        s.sock = open_udp(s.local_port);
        if (s.sock < 0) {
            log_line("[%s] bind impossible sur le port %d", s.name, s.local_port);
            continue;
        }
        log_line("[%s] %s:%d via port local %d", s.name, s.ip, s.port, s.local_port);
        ok++;
    }
    return ok;
}

static void* proxy_thread(void*) {
    static unsigned char buf[BUF_SIZE];

    g_listen = open_udp(LISTEN_PORT);
    if (g_listen < 0) {
        log_line("bind impossible sur le port %d", LISTEN_PORT);
        notify("mc_lan_proxy: port 19132 indisponible");
        return NULL;
    }

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen, &rfds);
        int maxfd = g_listen;
        for (int i = 0; i < g_count; i++) {
            if (g_servers[i].sock >= 0) {
                FD_SET(g_servers[i].sock, &rfds);
                if (g_servers[i].sock > maxfd) maxfd = g_servers[i].sock;
            }
        }

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        // 1) pings de decouverte LAN
        if (FD_ISSET(g_listen, &rfds)) {
            sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(g_listen, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n >= 25 && (buf[0] == 0x01 || buf[0] == 0x02)) {
                for (int i = 0; i < g_count; i++) send_pong(i, &from, buf);
            }
        }

        // 2) relais joueur <-> serveur
        for (int i = 0; i < g_count; i++) {
            Server& s = g_servers[i];
            if (s.sock < 0 || !FD_ISSET(s.sock, &rfds)) continue;

            sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(s.sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;

            if (same_addr(from, s.remote)) {
                if (s.has_client)
                    sendto(s.sock, buf, (size_t)n, 0, (sockaddr*)&s.client, sizeof(s.client));
            } else {
                s.client = from;
                s.has_client = true;
                sendto(s.sock, buf, (size_t)n, 0, (sockaddr*)&s.remote, sizeof(s.remote));
            }
        }
    }

    for (int i = 0; i < g_count; i++) {
        if (g_servers[i].sock >= 0) { close(g_servers[i].sock); g_servers[i].sock = -1; }
    }
    close(g_listen);
    g_listen = -1;
    return NULL;
}

// ---------------------------------------------------------------- entree PRX
static void start_proxy() {
    if (g_running) return;

    notify("mc_lan_proxy: demarrage...");   // premiere ligne : prouve que le PRX tourne

    mkdir(CONFIG_DIR, 0777);
    log_line("---- start ----");
    load_ini();
    int ok = setup_sockets();

    char msg[160];
    if (g_count == 0) {
        notify("mc_lan_proxy charge - aucun serveur dans servers.ini");
    } else {
        snprintf(msg, sizeof(msg), "mc_lan_proxy charge - %d/%d serveur(s) actif(s)", ok, g_count);
        notify(msg);
    }

    g_running = true;
    if (pthread_create(&g_thread, NULL, proxy_thread, NULL) != 0) {
        g_running = false;
        log_line("pthread_create a echoue");
        notify("mc_lan_proxy: erreur thread");
    }
}

static void stop_proxy() {
    if (g_running) {
        g_running = false;
        pthread_join(g_thread, NULL);
    }
}

// Points d'entree GoldHEN (plugin_load / plugin_unload)
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

// Points d'entree PRX classiques (chargement direct par le systeme)
extern "C" __attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args; (void)argp;
    start_proxy();
    return 0;
}

extern "C" __attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args; (void)argp;
    stop_proxy();
    return 0;
}
