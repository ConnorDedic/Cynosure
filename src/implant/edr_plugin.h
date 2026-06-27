#ifndef EDR_PLUGIN_H
#define EDR_PLUGIN_H

/*
 * edr_plugin.h — ABI contract every plugin DLL must satisfy.
 *
 * Every plugin DLL must export exactly one symbol:
 *     edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out_manifest);
 *
 * The dispatcher calls this immediately after dlopen(). The plugin fills in
 * the manifest and returns EDR_OK. The dispatcher then calls init() before
 * routing any commands to the plugin.
 */

#include "edr_types.h"

/* -------------------------------------------------------------------------
 * Forward declarations for all interface structs.
 * -------------------------------------------------------------------------*/
typedef struct edr_iface_comm        edr_iface_comm_t;
typedef struct edr_iface_file_ops    edr_iface_file_ops_t;
typedef struct edr_iface_scan        edr_iface_scan_t;
typedef struct edr_iface_event       edr_iface_event_t;
typedef struct edr_iface_remediation edr_iface_remediation_t;
typedef struct edr_iface_config      edr_iface_config_t;
typedef struct edr_iface_health      edr_iface_health_t;
typedef struct edr_iface_auth        edr_iface_auth_t;

/* -------------------------------------------------------------------------
 * Plugin services — provided BY the dispatcher TO each plugin at init time.
 * Plugins use these to log, allocate, and emit events without coupling to
 * specific implementations.
 * -------------------------------------------------------------------------*/
typedef struct {
    edr_log_fn_t log;

    /* Dispatcher-managed allocator — use these instead of malloc/free so
     * the dispatcher can track and bound plugin memory usage. */
    void *(*alloc)(size_t bytes);
    void  (*free)(void *ptr);

    /* Emit a structured telemetry event back to the dispatcher event bus.
     * Plugins call this rather than calling the event interface directly. */
    edr_status_t (*emit_event)(const char *event_type, const char *json_payload);
} edr_plugin_services_t;

/* -------------------------------------------------------------------------
 * Plugin manifest — filled by the plugin, read by the dispatcher.
 * -------------------------------------------------------------------------*/
#define EDR_PLUGIN_NAME_LEN    64
#define EDR_PLUGIN_VENDOR_LEN  64

typedef struct {
    /* ABI version this plugin was compiled against. */
    edr_version_t abi_version;

    /* Human-readable identity. */
    char name[EDR_PLUGIN_NAME_LEN];
    char vendor[EDR_PLUGIN_VENDOR_LEN];
    edr_version_t plugin_version;

    /* Bitmask of EDR_CAP_* flags this plugin implements. */
    uint32_t capabilities;

    /* Priority for capability routing (lower = preferred, 0 = highest).
     * When multiple plugins implement the same capability, the dispatcher
     * tries the lowest-priority-number plugin first and falls back on error. */
    uint8_t priority;

    /* Lifecycle — dispatcher calls these in order: init → [use] → shutdown. */
    edr_status_t (*init)(const edr_plugin_services_t *services,
                         const edr_agent_identity_t  *identity);
    edr_status_t (*shutdown)(void);

    /* Interface accessors — return NULL if capability not implemented.
     * Pointers remain valid until shutdown() returns. */
    const edr_iface_comm_t        *(*get_comm)(void);
    const edr_iface_file_ops_t    *(*get_file_ops)(void);
    const edr_iface_scan_t        *(*get_scan)(void);
    const edr_iface_event_t       *(*get_event)(void);
    const edr_iface_remediation_t *(*get_remediation)(void);
    const edr_iface_config_t      *(*get_config)(void);
    const edr_iface_health_t      *(*get_health)(void);
    const edr_iface_auth_t        *(*get_auth)(void);

} edr_plugin_manifest_t;

/* -------------------------------------------------------------------------
 * The one symbol every plugin DLL must export.
 * -------------------------------------------------------------------------*/
typedef edr_status_t (*edr_plugin_entry_fn_t)(edr_plugin_manifest_t *out);

#define EDR_PLUGIN_ENTRY_SYMBOL  "edr_plugin_entry"

#endif /* EDR_PLUGIN_H */
