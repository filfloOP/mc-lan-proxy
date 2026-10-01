#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 19132
#define BUFFER_SIZE 1024
#define MAX_SERVERS 10

struct ServerInfo {
    char name[64];
    char ip[64];
    int port;
};

static ServerInfo g_servers[MAX_SERVERS];
static int g_server_count = 0;
static pthread_t g_thread;
static volatile bool g_running = false;
static int g_sock = -1;

static const unsigned char RAKNET_MAGIC[] = {
    0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78
};

void parse_ini_file(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) return;

    char line[256];
    g_server_count = 0;
    ServerInfo current;
    memset(&current, 0, sizeof(current));
    current.port = 19132;

    while (fgets(line, sizeof(line), file) && g_server_count < MAX_SERVERS) {
        line[strcspn(line, "\r\n")] = 0;

        if (line[0] == '[' || line[0] == '#' || line[0] == ';' || line[0] == '\0') {
            if (current.name[0] != 0 && current.ip[0] != 0) {
                g_servers[g_server_count++] = current;
                memset(&current, 0, sizeof(current));
                current.port = 19132;
            }
            continue;
        }

        char key[64], val[128];
        if (sscanf(line, "%63[^=]=%127s", key, val) == 2) {
            if (strcmp(key, "name") == 0) strncpy(current.name, val, sizeof(current.name) - 1);
            else if (strcmp(key, "ip") == 0) strncpy(current.ip, val, sizeof(current.ip) - 1);
            else if (strcmp(key, "port") == 0) current.port = atoi(val);
        }
    }

    if (current.name[0] != 0 && current.ip[0] != 0 && g_server_count < MAX_SERVERS) {
        g_servers[g_server_count++] = current;
    }

    fclose(file);
}

void handle_raknet_ping(int sock, struct sockaddr_in* client_addr, socklen_t addr_len, unsigned char* buf, int len) {
    if (len < 33) return;

    uint64_t ping_time;
    memcpy(&ping_time, buf + 1, 8);

    for (int i = 0; i < g_server_count; i++) {
        unsigned char response[1024];
        int pos = 0;

        response[pos++] = 0x1c;
        memcpy(response + pos, &ping_time, 8); pos += 8;

        uint64_t server_guid = 0x1122334455667788ULL + i;
        memcpy(response + pos, &server_guid, 8); pos += 8;
        memcpy(response + pos, RAKNET_MAGIC, 16); pos += 16;

        char str_payload[512];
        snprintf(str_payload, sizeof(str_payload),
                 "MCPE;%s;589;1.20.10;0;10;%llu;LAN Server PS4;Survival;1;%d;19133;",
                 g_servers[i].name, (unsigned long long)server_guid, g_servers[i].port);

        uint16_t str_len = htons((uint16_t)strlen(str_payload));
        memcpy(response + pos, &str_len, 2); pos += 2;
        memcpy(response + pos, str_payload, strlen(str_payload)); pos += strlen(str_payload);

        sendto(sock, response, pos, 0, (struct sockaddr*)client_addr, addr_len);
    }
}

void* lan_proxy_thread(void* arg) {
    parse_ini_file("/data/servers.ini");

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) return NULL;

    int opt = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(g_sock);
        g_sock = -1;
        return NULL;
    }

    unsigned char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (g_running) {
        int bytes = recvfrom(g_sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (bytes > 0 && (buffer[0] == 0x01 || buffer[0] == 0x02)) {
            handle_raknet_ping(g_sock, &client_addr, addr_len, buffer, bytes);
        }
    }

    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }

    return NULL;
}

extern "C" __attribute__((visibility("default"))) int module_start(size_t args, const void *argp) {
    if (!g_running) {
        g_running = true;
        pthread_create(&g_thread, NULL, lan_proxy_thread, NULL);
    }
    return 0;
}

extern "C" __attribute__((visibility("default"))) int module_stop(size_t args, const void *argp) {
    if (g_running) {
        g_running = false;
        if (g_sock >= 0) {
            close(g_sock);
            g_sock = -1;
        }
        pthread_join(g_thread, NULL);
    }
    return 0;
}
