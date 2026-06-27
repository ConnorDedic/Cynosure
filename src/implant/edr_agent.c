/* Required for gethostname on strict C11 */
#define _POSIX_C_SOURCE 200809L

/*
 * edr_agent.c — EDR agent entry point.
 *
 * Responsibilities:
 *   - Parse config / environment
 *   - Build edr_agent_identity_t
 *   - Construct and start the dispatcher
 *   - Handle OS signals for graceful shutdown
 *
 * This file intentionally contains zero business logic. Everything routes
 * through the dispatcher and its loaded plugins.
 */

#include "edr_dispatcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>
static inline int  edr_getpid(void)                  { return (int)GetCurrentProcessId(); }
static inline void edr_gethostname(char *b, size_t n) { DWORD s=(DWORD)n; GetComputerNameA(b,&s); }
#else
#  include <unistd.h>
#  include <sys/utsname.h>
static inline int  edr_getpid(void)                  { return (int)getpid(); }
static inline void edr_gethostname(char *b, size_t n) { gethostname(b, n-1); }
#endif

/* -------------------------------------------------------------------------
 * Global dispatcher handle for signal handler access
 * -------------------------------------------------------------------------*/
static edr_dispatcher_t *g_dispatcher = NULL;
edr_dispatcher_t *g_edr_dispatcher = NULL;

/* -------------------------------------------------------------------------
 * Signal handler — request graceful shutdown
 * -------------------------------------------------------------------------*/
static void handle_signal(int sig)
{
    (void)sig;
    if (g_dispatcher) {
        edr_dispatcher_stop(g_dispatcher);
    }
}

/* -------------------------------------------------------------------------
 * Simple stderr logger
 * -------------------------------------------------------------------------*/
static void agent_log(edr_log_level_t level, const char *component,
                       const char *fmt, ...)
{
    static const char *labels[] = { "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL" };
    const char *label = labels[level < 5 ? level : 4];

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[%s][%s] %s\n", label, component, buf);
}

/* -------------------------------------------------------------------------
 * Build the agent identity from the running system
 * -------------------------------------------------------------------------*/
static void build_identity(edr_agent_identity_t *id)
{
    memset(id, 0, sizeof(*id));

    /* Agent ID — read from a persisted file or generate a UUID on first boot. */
    const char *id_path = "/etc/edr/agent_id";
    FILE *f = fopen(id_path, "r");
    if (f) {
        if (fgets(id->agent_id, sizeof(id->agent_id), f)) {
            /* Strip trailing newline. */
            size_t n = strlen(id->agent_id);
            if (n > 0 && id->agent_id[n-1] == '\n') id->agent_id[n-1] = '\0';
        }
        fclose(f);
    }

    if (id->agent_id[0] == '\0') {
        /* Fallback: use hostname + pid as a temporary ID. */
        snprintf(id->agent_id, sizeof(id->agent_id),
                 "agent-%d-%d", edr_getpid(), (int)time(NULL));
        agent_log(EDR_LOG_WARN, "agent",
                  "No persisted agent_id found at %s — using temp id '%s'",
                  id_path, id->agent_id);
    }

    /* Hostname */
    edr_gethostname(id->hostname, sizeof(id->hostname));

    /* Platform */
#if defined(__linux__)
    id->platform = EDR_PLATFORM_LINUX;
#elif defined(__APPLE__)
    id->platform = EDR_PLATFORM_MACOS;
#elif defined(_WIN32)
    id->platform = EDR_PLATFORM_WINDOWS;
#else
    id->platform = 0;
#endif

    id->agent_version.major = EDR_INTERFACE_VERSION_MAJOR;
    id->agent_version.minor = EDR_INTERFACE_VERSION_MINOR;
    id->agent_version.patch = EDR_INTERFACE_VERSION_PATCH;
}

/* -------------------------------------------------------------------------
 * Build dispatcher config from environment / config file
 * -------------------------------------------------------------------------*/
static void build_config(edr_dispatcher_config_t *cfg,
                          const edr_agent_identity_t *identity)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->identity = *identity;
    cfg->log      = agent_log;

    /* Controller endpoint — prefer environment variables for container
     * deployments; fall back to compiled-in defaults. */
    const char *ctrl_url  = getenv("EDR_CONTROLLER_URL");
    const char *ctrl_port = getenv("EDR_CONTROLLER_PORT");
    const char *plugin_dir = getenv("EDR_PLUGIN_DIR");

    /* CB_IP / CB_PORT are injected by the builder at compile time.
     * Env vars override them for container / testing deployments. */
#ifndef CB_IP
#  define CB_IP "127.0.0.1"
#endif
#ifndef CB_PORT
#  define CB_PORT 4444
#endif

    strncpy(cfg->controller_endpoint.url,
            ctrl_url ? ctrl_url : "http://" CB_IP,
            sizeof(cfg->controller_endpoint.url) - 1);

    cfg->controller_endpoint.port =
        (uint16_t)(ctrl_port ? atoi(ctrl_port) : CB_PORT);

    cfg->controller_endpoint.connect_timeout_ms = 10000;
    cfg->controller_endpoint.recv_timeout_ms    = 60000;

    /* TLS — paths to PEM files, read from environment. */
    cfg->controller_endpoint.ca_cert_pem     = getenv("EDR_CA_CERT");
    cfg->controller_endpoint.client_cert_pem = getenv("EDR_CLIENT_CERT");
    cfg->controller_endpoint.client_key_pem  = getenv("EDR_CLIENT_KEY");

#ifdef _WIN32
    /* On Windows, default to the directory containing the agent exe so that
     * comm DLLs can be dropped alongside it without any extra setup. */
    static char win_plugin_dir[MAX_PATH];
    if (!plugin_dir) {
        DWORD n = GetModuleFileNameA(NULL, win_plugin_dir, (DWORD)sizeof(win_plugin_dir));
        if (n > 0) {
            char *sep = strrchr(win_plugin_dir, '\\');
            if (sep) *(sep + 1) = '\0'; /* keep trailing backslash, drop exe name */
            else     strcpy(win_plugin_dir, ".\\");
        } else {
            strcpy(win_plugin_dir, ".\\");
        }
        plugin_dir = win_plugin_dir;
    }
#endif
    cfg->plugin_dir = plugin_dir ? plugin_dir : "/opt/edr/plugins";

    cfg->heartbeat_interval_ms    = 30000;   /* 30 s */
    cfg->health_report_interval_ms = 300000; /* 5 min */

    cfg->max_plugin_cpu_percent = 10.0f;         /* 10 % CPU cap per plugin */
    cfg->max_plugin_mem_bytes   = 256ULL << 20;  /* 256 MiB */
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    agent_log(EDR_LOG_INFO, "agent", "EDR agent starting");

    /* Build identity and config. */
    edr_agent_identity_t identity;
    build_identity(&identity);

    agent_log(EDR_LOG_INFO, "agent",
              "Identity: id='%s' hostname='%s'",
              identity.agent_id, identity.hostname);

    edr_dispatcher_config_t cfg;
    build_config(&cfg, &identity);

    /* Create dispatcher. */
    g_dispatcher = edr_dispatcher_create(&cfg);
    if (!g_dispatcher) {
        agent_log(EDR_LOG_FATAL, "agent", "Failed to create dispatcher");
        return 1;
    }
    g_edr_dispatcher = g_dispatcher;  /* Export for VPN module */

    /* Register dispatcher with VPN module after plugins load.
     * This is done in a callback after start(). */

    /* Register OS signal handlers for graceful shutdown. */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
#if !defined(_WIN32)
    signal(SIGHUP,  handle_signal);  /* reload — treat as restart for now */
    signal(SIGPIPE, SIG_IGN);        /* ignore broken pipe from transport */
#endif

    /* Start blocks until stop() is called (e.g. from signal handler). */
    edr_status_t st = edr_dispatcher_start(g_dispatcher);

    if (st != EDR_OK) {
        agent_log(EDR_LOG_ERROR, "agent",
                  "Dispatcher exited with status %d", st);
    }

    edr_dispatcher_destroy(g_dispatcher);
    g_dispatcher = NULL;

    agent_log(EDR_LOG_INFO, "agent", "EDR agent stopped cleanly");
    return (st == EDR_OK) ? 0 : 1;
}
