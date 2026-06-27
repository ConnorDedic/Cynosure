/*
 * file_http_comm.c — File transfer via HTTP server
 *
 * Runs HTTP server on port 8888:
 * - POST /upload/<filename> → receives and writes file
 * - GET /download/<filename> → reads and sends file
 */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "kernel32.lib")
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <pthread.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "edr_plugin.h"
#include "edr_interfaces.h"

/* Global state */
static const edr_plugin_services_t *g_svc = NULL;
static int g_server_socket = -1;
static int g_server_running = 0;

#ifdef _WIN32
    static HANDLE g_server_thread = NULL;
#else
    static pthread_t g_server_thread;
#endif

/* HTTP server implementation */
#ifdef _WIN32
static DWORD WINAPI http_server_thread(LPVOID arg) {
#else
static void *http_server_thread(void *arg) {
#endif
    (void)arg;
    struct sockaddr_in server_addr, client_addr;
    int client_socket;
    char request[4096];
    char response[8192];

    g_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_server_socket < 0) {
        if (g_svc && g_svc->log) g_svc->log(EDR_LOG_ERROR, "file_http", "socket() failed");
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(8888);

    int reuse = 1;
    setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    if (bind(g_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (g_svc && g_svc->log) g_svc->log(EDR_LOG_ERROR, "file_http", "bind(8888) failed");
#ifdef _WIN32
        closesocket(g_server_socket);
        return 0;
#else
        close(g_server_socket);
        return NULL;
#endif
    }

    listen(g_server_socket, 5);
    if (g_svc && g_svc->log) g_svc->log(EDR_LOG_INFO, "file_http", "HTTP server listening on :8888");

    while (g_server_running) {
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif

        client_socket = accept(g_server_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) continue;

        memset(request, 0, sizeof(request));
        int n = recv(client_socket, request, sizeof(request) - 1, 0);
        if (n <= 0) {
#ifdef _WIN32
            closesocket(client_socket);
#else
            close(client_socket);
#endif
            continue;
        }

        /* Parse HTTP request: GET /download/file.txt or POST /upload/file.txt */
        char method[16], path[512];
        sscanf(request, "%15s %511s", method, path);

        if (strstr(request, "POST") && strstr(path, "/upload/")) {
            /* Extract filename from /upload/filename */
            char *filename = path + 8;  /* skip "/upload/" */
            char *end = strchr(filename, ' ');
            if (end) *end = '\0';

            /* Find Content-Length */
            char *cl_start = strstr(request, "Content-Length: ");
            int content_len = 0;
            if (cl_start) {
                sscanf(cl_start, "Content-Length: %d", &content_len);
            }

            /* Find body (after double CRLF) */
            char *body_start = strstr(request, "\r\n\r\n");
            if (!body_start) body_start = strstr(request, "\n\n");
            if (body_start) {
                body_start += 4;
                FILE *f = fopen(filename, "wb");
                if (f) {
                    int body_len = (body_start - request) + content_len > n ? n - (body_start - request) : content_len;
                    fwrite(body_start, 1, body_len, f);
                    fclose(f);

                    snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
                    if (g_svc && g_svc->log) {
                        g_svc->log(EDR_LOG_INFO, "file_http", "Received file: %s (%d bytes)", filename, body_len);
                    }
                } else {
                    snprintf(response, sizeof(response),
                        "HTTP/1.1 500 Internal Error\r\nContent-Length: 0\r\n\r\n");
                }
            } else {
                snprintf(response, sizeof(response),
                    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            }

            send(client_socket, response, strlen(response), 0);

        } else if (strstr(request, "GET") && strstr(path, "/download/")) {
            /* Extract filename from /download/filename */
            char *filename = path + 10;  /* skip "/download/" */
            char *end = strchr(filename, ' ');
            if (end) *end = '\0';

            FILE *f = fopen(filename, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);

                snprintf(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nContent-Type: application/octet-stream\r\n\r\n",
                    file_size);
                send(client_socket, response, strlen(response), 0);

                char buf[4096];
                while (1) {
                    int len = fread(buf, 1, sizeof(buf), f);
                    if (len <= 0) break;
                    send(client_socket, buf, len, 0);
                }
                fclose(f);

                if (g_svc && g_svc->log) {
                    g_svc->log(EDR_LOG_INFO, "file_http", "Sent file: %s (%ld bytes)", filename, file_size);
                }
            } else {
                snprintf(response, sizeof(response),
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                send(client_socket, response, strlen(response), 0);
            }
        } else {
            snprintf(response, sizeof(response),
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            send(client_socket, response, strlen(response), 0);
        }

#ifdef _WIN32
        closesocket(client_socket);
#else
        close(client_socket);
#endif
    }

#ifdef _WIN32
    if (g_server_socket >= 0) closesocket(g_server_socket);
    return 0;
#else
    if (g_server_socket >= 0) close(g_server_socket);
    return NULL;
#endif
}

/* HTTP file interface implementation */
static edr_status_t fh_connect(const edr_endpoint_t *ep,
                               edr_completion_cb_t cb,
                               void *ctx) {
    (void)ep;

    /* Start HTTP server thread if not already running */
    if (!g_server_running) {
        g_server_running = 1;

#ifdef _WIN32
        g_server_thread = CreateThread(NULL, 0, http_server_thread, NULL, 0, NULL);
        if (!g_server_thread) {
            g_server_running = 0;
            if (cb) cb(ctx, EDR_ERR_GENERIC, NULL);
            return EDR_ERR_GENERIC;
        }
#else
        if (pthread_create(&g_server_thread, NULL, http_server_thread, NULL) != 0) {
            g_server_running = 0;
            if (cb) cb(ctx, EDR_ERR_GENERIC, NULL);
            return EDR_ERR_GENERIC;
        }
#endif
    }

    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fh_disconnect(edr_completion_cb_t cb, void *ctx) {
    g_server_running = 0;

    if (g_server_socket >= 0) {
#ifdef _WIN32
        closesocket(g_server_socket);
#else
        close(g_server_socket);
#endif
        g_server_socket = -1;
    }

    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fh_send(const edr_message_t *msg,
                            edr_completion_cb_t cb,
                            void *ctx) {
    (void)msg;
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fh_set_recv_handler(edr_recv_cb_t handler, void *ctx) {
    (void)handler;
    (void)ctx;
    return EDR_OK;
}

static edr_status_t fh_heartbeat(edr_completion_cb_t cb, void *ctx) {
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_iface_comm_t g_iface_http = {
    .connect = fh_connect,
    .disconnect = fh_disconnect,
    .send = fh_send,
    .set_recv_handler = fh_set_recv_handler,
    .heartbeat = fh_heartbeat,
};

/* Plugin manifest accessors */
static edr_status_t fh_init(const edr_plugin_services_t *svc,
                            const edr_agent_identity_t *identity) {
    (void)identity;
    g_svc = svc;
    if (svc && svc->log) {
        svc->log(EDR_LOG_INFO, "file_http", "File HTTP module initialized");
    }
    return EDR_OK;
}

static edr_status_t fh_shutdown(void) {
    g_svc = NULL;
    return EDR_OK;
}

static const edr_iface_comm_t *fh_get_comm(void) {
    return &g_iface_http;
}

static const edr_iface_file_ops_t *fh_get_file_ops(void) { return NULL; }
static const edr_iface_scan_t *fh_get_scan(void) { return NULL; }
static const edr_iface_event_t *fh_get_event(void) { return NULL; }
static const edr_iface_remediation_t *fh_get_remediation(void) { return NULL; }
static const edr_iface_config_t *fh_get_config(void) { return NULL; }
static const edr_iface_health_t *fh_get_health(void) { return NULL; }
static const edr_iface_auth_t *fh_get_auth(void) { return NULL; }

/* Plugin entry point */
__declspec(dllexport)
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out) {
    if (!out) return EDR_ERR_INVALID_ARG;

    out->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    strncpy(out->name, "file_http", EDR_PLUGIN_NAME_LEN - 1);
    strncpy(out->vendor, "cynosure", EDR_PLUGIN_VENDOR_LEN - 1);

    out->plugin_version.major = 0;
    out->plugin_version.minor = 1;
    out->plugin_version.patch = 0;

    out->capabilities = EDR_CAP_COMM_TRANSPORT;
    out->priority = 51;

    out->init = fh_init;
    out->shutdown = fh_shutdown;
    out->get_comm = fh_get_comm;
    out->get_file_ops = fh_get_file_ops;
    out->get_scan = fh_get_scan;
    out->get_event = fh_get_event;
    out->get_remediation = fh_get_remediation;
    out->get_config = fh_get_config;
    out->get_health = fh_get_health;
    out->get_auth = fh_get_auth;

    return EDR_OK;
}
