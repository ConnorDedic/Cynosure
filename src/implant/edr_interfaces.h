#ifndef EDR_INTERFACES_H
#define EDR_INTERFACES_H

/*
 * edr_interfaces.h — Concrete vtable structs for all eight EDR interfaces.
 *
 * DESIGN RULES:
 *   1. Every function pointer in these structs may be NULL if the plugin
 *      partially implements the capability — the dispatcher checks before
 *      calling and returns EDR_ERR_NOT_IMPL.
 *   2. All blocking work MUST be done asynchronously. Functions that kick off
 *      I/O, scans, or uploads return EDR_OK immediately and fire the supplied
 *      edr_completion_cb_t when done.
 *   3. String arguments are UTF-8. Paths use forward slashes on all platforms.
 *   4. Plugins must not free memory passed in by the dispatcher; dispatcher
 *      must not free memory returned by plugins unless documented otherwise.
 */

#include "edr_types.h"

/* =========================================================================
 * 1. ICommTransport — network transport abstraction
 * =========================================================================*/

#define EDR_ENDPOINT_URL_LEN  512

typedef struct {
    char url[EDR_ENDPOINT_URL_LEN];
    uint16_t port;
    uint32_t connect_timeout_ms;
    uint32_t recv_timeout_ms;
    /* TLS/mTLS config — plugin interprets these opaquely. */
    const char *ca_cert_pem;        /* NULL = use system CA bundle */
    const char *client_cert_pem;    /* NULL = no client cert */
    const char *client_key_pem;
} edr_endpoint_t;

typedef enum {
    EDR_MSG_COMMAND  = 1,   /* Inbound command from controller */
    EDR_MSG_RESPONSE = 2,   /* Outbound response to controller */
    EDR_MSG_EVENT    = 3,   /* Outbound telemetry event */
    EDR_MSG_HEARTBEAT= 4,
} edr_msg_type_t;

typedef struct {
    edr_msg_type_t  type;
    uint64_t        seq;            /* Monotonic sequence number */
    uint64_t        correlation_id; /* Links response to originating command */
    char            command_id[64]; /* e.g. "scan.process", "file.upload" */
    edr_buf_t       payload;        /* JSON or binary blob per command_id */
    time_t          timestamp;
} edr_message_t;

/* Callback invoked for every inbound message from the controller. */
typedef void (*edr_recv_cb_t)(const edr_message_t *msg, void *ctx);

struct edr_iface_comm {
    /*
     * connect — establish transport channel to endpoint.
     * Non-blocking: fires cb when connected or failed.
     */
    edr_status_t (*connect)(const edr_endpoint_t *ep,
                            edr_completion_cb_t   cb,
                            void                 *ctx);

    /* Graceful disconnect. Flushes any pending sends first. */
    edr_status_t (*disconnect)(edr_completion_cb_t cb, void *ctx);

    /*
     * send — enqueue a message for delivery.
     * The plugin owns delivery guarantees (retry, ordering) internally.
     * cb fires when the message is acknowledged by the far end.
     */
    edr_status_t (*send)(const edr_message_t *msg,
                         edr_completion_cb_t  cb,
                         void                *ctx);

    /*
     * set_recv_handler — register the dispatcher's inbound message callback.
     * Only one handler at a time; replaces any previous registration.
     */
    edr_status_t (*set_recv_handler)(edr_recv_cb_t handler, void *ctx);

    /*
     * heartbeat — send a liveness ping to the controller.
     * The dispatcher calls this on its own timer; plugins may also
     * initiate internally but must not suppress dispatcher-driven heartbeats.
     */
    edr_status_t (*heartbeat)(edr_completion_cb_t cb, void *ctx);

    /* Query current transport state without side effects. */
    int (*is_connected)(void);
};

/* =========================================================================
 * 2. IFileOps — collect, upload, download, install
 * =========================================================================*/

typedef struct {
    char      path[4096];
    uint64_t  size_bytes;
    time_t    mtime;
    uint8_t   sha256[32];   /* Computed by plugin during collect */
    int       is_directory;
} edr_file_meta_t;

typedef struct {
    /* Remote object key / URL the plugin resolves internally. */
    char   object_key[512];
    /* Expected SHA-256 of the remote file — plugin verifies after download. */
    uint8_t expected_sha256[32];
    /* If nonzero, plugin must verify a digital signature on the payload. */
    int    require_signature;
    const char *signature_pubkey_pem;
} edr_cloud_ref_t;

typedef struct {
    char install_path[4096]; /* Absolute destination path */
    int  executable;         /* chmod +x / mark executable on Windows */
    int  run_after_install;  /* Launch immediately after writing */
    /* Args passed if run_after_install is set. NULL-terminated array. */
    const char **argv;
} edr_install_opts_t;

/* Progress callback for long transfers: bytes_done of bytes_total. */
typedef void (*edr_progress_cb_t)(uint64_t bytes_done, uint64_t bytes_total,
                                   void *ctx);

struct edr_iface_file_ops {
    /*
     * collect — stat + hash a local file or directory tree.
     * Result passed as edr_file_meta_t* array via completion cb data.
     * Caller must free via plugin_services->free().
     */
    edr_status_t (*collect)(const char          *local_path,
                            int                  recursive,
                            edr_completion_cb_t  cb,
                            void                *ctx);

    /*
     * upload — stream a local file to the cloud endpoint.
     * Plugin handles multipart, resumable, and retry internally.
     */
    edr_status_t (*upload)(const char          *local_path,
                           const char          *object_key,
                           edr_progress_cb_t    progress_cb,
                           void                *progress_ctx,
                           edr_completion_cb_t  cb,
                           void                *ctx);

    /*
     * download — fetch a cloud object to a local temp path.
     * Plugin verifies SHA-256 after download; fires cb with temp path string.
     */
    edr_status_t (*download)(const edr_cloud_ref_t *ref,
                             edr_progress_cb_t      progress_cb,
                             void                  *progress_ctx,
                             edr_completion_cb_t    cb,
                             void                  *ctx);

    /*
     * install — write a previously-downloaded file to its final destination
     * and optionally execute it.
     * Plugin verifies signature before writing if require_signature is set.
     */
    edr_status_t (*install)(const char              *tmp_path,
                            const edr_install_opts_t *opts,
                            edr_completion_cb_t       cb,
                            void                     *ctx);

    /* verify_integrity — re-hash a local file and compare to expected. */
    edr_status_t (*verify_integrity)(const char *path,
                                     const uint8_t expected_sha256[32],
                                     int          *out_match);

    /* delete_file — securely wipe and remove a local file. */
    edr_status_t (*delete_file)(const char *path, edr_completion_cb_t cb,
                                void *ctx);
};

/* =========================================================================
 * 3. IScanEngine — process, path, memory, registry
 * =========================================================================*/

typedef enum {
    EDR_SCAN_PROCESS  = 1,
    EDR_SCAN_PATH     = 2,
    EDR_SCAN_MEMORY   = 3,
    EDR_SCAN_REGISTRY = 4,   /* Windows only; plugins return EDR_ERR_UNSUPPORTED on others */
    EDR_SCAN_NETWORK  = 5,   /* Active connection table scan */
} edr_scan_type_t;

typedef struct {
    edr_scan_type_t type;
    /* Scope selectors — interpretation depends on scan type:
     *   PROCESS:  CSV of PIDs ("0" = all), or process name globs.
     *   PATH:     absolute paths or glob patterns, NULL-terminated array.
     *   MEMORY:   same as PROCESS.
     *   REGISTRY: root key paths, NULL-terminated array (Windows).
     *   NETWORK:  ignored (scans entire connection table).
     */
    const char **targets;      /* NULL-terminated; NULL means "scan all" */
    int          recursive;    /* For PATH scans */
    uint32_t     max_depth;    /* 0 = unlimited */
    uint32_t     timeout_ms;   /* 0 = no timeout */
} edr_scan_request_t;

typedef enum {
    EDR_SEVERITY_INFO     = 0,
    EDR_SEVERITY_LOW      = 1,
    EDR_SEVERITY_MEDIUM   = 2,
    EDR_SEVERITY_HIGH     = 3,
    EDR_SEVERITY_CRITICAL = 4,
} edr_severity_t;

#define EDR_IOC_MAX_LEN  512

typedef struct {
    edr_severity_t severity;
    char  rule_id[128];          /* YARA rule, sigma rule, or internal ID */
    char  ioc[EDR_IOC_MAX_LEN];  /* Matched indicator (hash, path, regex, ...) */
    char  description[512];
    char  process_name[256];
    uint32_t pid;
    char  file_path[4096];
    char  registry_key[1024];
    time_t detected_at;
    /* Raw evidence blob — YARA match bytes, memory dump excerpt, etc. */
    edr_buf_t evidence;
} edr_finding_t;

typedef struct {
    uint32_t        finding_count;
    edr_finding_t  *findings;   /* Heap-allocated; dispatcher frees via services->free */
    uint64_t        objects_scanned;
    uint32_t        duration_ms;
    edr_status_t    scan_status;
} edr_scan_result_t;

/* Streaming callback — fires for each finding as it is detected.
 * Allows the dispatcher to emit events without waiting for full scan completion. */
typedef void (*edr_finding_cb_t)(const edr_finding_t *finding, void *ctx);

struct edr_iface_scan {
    /*
     * scan — start an asynchronous scan.
     * finding_cb fires per-finding during the scan.
     * completion_cb fires once with edr_scan_result_t* as data.
     */
    edr_status_t (*scan)(const edr_scan_request_t *req,
                         edr_finding_cb_t          finding_cb,
                         void                     *finding_ctx,
                         edr_completion_cb_t       completion_cb,
                         void                     *completion_ctx);

    /* cancel — request cancellation of an in-progress scan. Best-effort. */
    edr_status_t (*cancel)(void);

    /* load_rules — push updated YARA/Sigma rule set to the engine. */
    edr_status_t (*load_rules)(const edr_buf_t *rules_blob,
                               const char      *format,   /* "yara", "sigma" */
                               edr_completion_cb_t cb,
                               void            *ctx);
};

/* =========================================================================
 * 4. IEventStream — structured telemetry pipeline
 * =========================================================================*/

typedef enum {
    EDR_EVENT_PROCESS_CREATE    = 1,
    EDR_EVENT_PROCESS_TERMINATE = 2,
    EDR_EVENT_FILE_CREATE       = 3,
    EDR_EVENT_FILE_MODIFY       = 4,
    EDR_EVENT_FILE_DELETE       = 5,
    EDR_EVENT_NETWORK_CONNECT   = 6,
    EDR_EVENT_NETWORK_LISTEN    = 7,
    EDR_EVENT_REGISTRY_WRITE    = 8,   /* Windows */
    EDR_EVENT_REGISTRY_DELETE   = 9,   /* Windows */
    EDR_EVENT_LOGON             = 10,
    EDR_EVENT_PRIVILEGE_ESC     = 11,
    EDR_EVENT_FINDING           = 12,  /* Scan finding forwarded as event */
    EDR_EVENT_AGENT_HEALTH      = 13,
    EDR_EVENT_CUSTOM            = 99,
} edr_event_type_t;

typedef struct {
    edr_event_type_t type;
    uint64_t         event_id;      /* Dispatcher-assigned monotonic ID */
    time_t           occurred_at;
    char             source[64];    /* Plugin name that emitted this */
    char             json[4096];    /* Structured payload; always valid JSON */
} edr_event_t;

/* Subscriber callback — called in dispatcher thread; must not block. */
typedef void (*edr_event_cb_t)(const edr_event_t *event, void *ctx);

struct edr_iface_event {
    /*
     * subscribe — register interest in one or more event types.
     * event_mask: bitmask of (1 << edr_event_type_t) values; 0 = all.
     * Returns a subscription handle via *out_handle.
     */
    edr_status_t (*subscribe)(uint64_t       event_mask,
                              edr_event_cb_t handler,
                              void          *ctx,
                              uint64_t      *out_handle);

    /* unsubscribe — remove a previously registered subscription. */
    edr_status_t (*unsubscribe)(uint64_t handle);

    /*
     * emit — publish an event to all matching subscribers and the upstream
     * transport queue. Dispatcher calls this; plugins use services->emit_event.
     */
    edr_status_t (*emit)(const edr_event_t *event);

    /*
     * flush — block until all queued events have been delivered upstream.
     * Used during graceful shutdown.
     */
    edr_status_t (*flush)(uint32_t timeout_ms);

    /* set_batch_size — tune how many events are coalesced per upstream send. */
    edr_status_t (*set_batch_size)(uint32_t n);
};

/* =========================================================================
 * 5. IRemediation — isolation, kill, block, restore
 * =========================================================================*/

typedef struct {
    uint32_t pid;
    char     process_name[256];
    char     image_path[4096];
} edr_process_ref_t;

typedef struct {
    char path[4096];
    char quarantine_id[64]; /* Opaque ID for restore operations */
} edr_quarantine_ref_t;

typedef struct {
    char   remote_ip[46];    /* IPv4 or IPv6 */
    uint16_t remote_port;
    char   local_ip[46];
    uint16_t local_port;
    char   protocol[8];      /* "tcp", "udp" */
} edr_network_ref_t;

struct edr_iface_remediation {
    /* Kill a running process. force=1 sends SIGKILL/TerminateProcess. */
    edr_status_t (*kill_process)(const edr_process_ref_t *proc,
                                 int                      force,
                                 edr_completion_cb_t      cb,
                                 void                    *ctx);

    /*
     * quarantine_file — move file to secure quarantine store, record metadata.
     * out_ref is filled with quarantine_id for later restore.
     */
    edr_status_t (*quarantine_file)(const char            *path,
                                    edr_quarantine_ref_t  *out_ref,
                                    edr_completion_cb_t    cb,
                                    void                  *ctx);

    /* restore_file — move quarantined file back to its original location. */
    edr_status_t (*restore_file)(const edr_quarantine_ref_t *ref,
                                 edr_completion_cb_t         cb,
                                 void                       *ctx);

    /* delete_quarantined — permanently destroy a quarantined file. */
    edr_status_t (*delete_quarantined)(const edr_quarantine_ref_t *ref,
                                       edr_completion_cb_t         cb,
                                       void                       *ctx);

    /*
     * block_network — install a host-based firewall rule to drop traffic
     * matching ref. rule_id is output for later removal.
     */
    edr_status_t (*block_network)(const edr_network_ref_t *ref,
                                  char                    *out_rule_id,
                                  size_t                   rule_id_len,
                                  edr_completion_cb_t      cb,
                                  void                    *ctx);

    /* unblock_network — remove a firewall rule installed by block_network. */
    edr_status_t (*unblock_network)(const char          *rule_id,
                                    edr_completion_cb_t  cb,
                                    void                *ctx);

    /*
     * isolate_host — full network isolation: block all traffic except
     * the EDR controller channel. Used for critical containment.
     */
    edr_status_t (*isolate_host)(edr_completion_cb_t cb, void *ctx);

    /* unisolate_host — remove host isolation. */
    edr_status_t (*unisolate_host)(edr_completion_cb_t cb, void *ctx);
};

/* =========================================================================
 * 6. IConfigSync — policy distribution and state reporting
 * =========================================================================*/

#define EDR_POLICY_KEY_LEN   128
#define EDR_POLICY_VAL_LEN   1024

typedef struct {
    char key[EDR_POLICY_KEY_LEN];
    char value[EDR_POLICY_VAL_LEN]; /* JSON-encoded */
} edr_policy_entry_t;

typedef struct {
    uint64_t          revision;
    time_t            issued_at;
    uint32_t          entry_count;
    edr_policy_entry_t *entries;
} edr_policy_t;

/* Callback fired when the plugin detects a new policy from the controller. */
typedef void (*edr_policy_cb_t)(const edr_policy_t *policy, void *ctx);

struct edr_iface_config {
    /* pull_policy — fetch current policy from controller. Async. */
    edr_status_t (*pull_policy)(edr_completion_cb_t cb, void *ctx);

    /* push_state — send current agent state snapshot to controller. */
    edr_status_t (*push_state)(const char         *state_json,
                               edr_completion_cb_t cb,
                               void               *ctx);

    /* set_policy_handler — called when a policy push arrives inbound. */
    edr_status_t (*set_policy_handler)(edr_policy_cb_t handler, void *ctx);

    /* apply_policy — apply a policy blob to the agent's local config store. */
    edr_status_t (*apply_policy)(const edr_policy_t *policy);

    /* get_current_revision — return the revision ID of the active policy. */
    edr_status_t (*get_current_revision)(uint64_t *out_revision);
};

/* =========================================================================
 * 7. IHealthReport — diagnostics, resource usage, integrity
 * =========================================================================*/

typedef struct {
    float    cpu_percent;        /* Agent process CPU over last interval */
    uint64_t mem_rss_bytes;
    uint64_t mem_virt_bytes;
    uint32_t open_file_handles;
    uint32_t queued_events;
    uint32_t queued_uploads;
    float    disk_io_mbps;
    float    net_io_mbps;
} edr_resource_stats_t;

typedef struct {
    char     plugin_name[EDR_PLUGIN_NAME_LEN];
    int      loaded;
    int      healthy;
    char     last_error[256];
    uint64_t calls_ok;
    uint64_t calls_err;
} edr_plugin_health_t;

typedef struct {
    edr_resource_stats_t  resources;
    uint32_t              plugin_count;
    edr_plugin_health_t  *plugins;       /* plugin_count entries */
    time_t                last_heartbeat;
    time_t                last_policy_pull;
    time_t                last_scan_completed;
    int                   host_isolated;
    edr_version_t         agent_version;
    char                  uptime_str[64];
} edr_health_report_t;

struct edr_iface_health {
    /* collect — build and return a full health snapshot. Synchronous. */
    edr_status_t (*collect)(edr_health_report_t *out);

    /*
     * self_integrity_check — verify the dispatcher binary and all loaded
     * plugin DLLs against stored hashes.
     * out_tampered is set to 1 if any mismatch is found.
     */
    edr_status_t (*self_integrity_check)(int *out_tampered);

    /* report — serialize and push a health report upstream. Async. */
    edr_status_t (*report)(edr_completion_cb_t cb, void *ctx);

    /* set_resource_limits — instruct the plugin to throttle if limits exceeded. */
    edr_status_t (*set_resource_limits)(float max_cpu_percent,
                                         uint64_t max_mem_bytes);
};

/* =========================================================================
 * 8. IAuthProvider — mutual auth, token lifecycle, certificate rotation
 * =========================================================================*/

#define EDR_TOKEN_LEN  2048

typedef struct {
    char     token[EDR_TOKEN_LEN];  /* Bearer token or session key */
    time_t   expires_at;
    int      is_valid;
} edr_auth_token_t;

struct edr_iface_auth {
    /*
     * authenticate — perform initial mutual auth with the controller.
     * On success, plugin stores the session internally and populates out_token.
     */
    edr_status_t (*authenticate)(const edr_agent_identity_t *identity,
                                  edr_auth_token_t           *out_token,
                                  edr_completion_cb_t         cb,
                                  void                       *ctx);

    /* refresh_token — renew a token before expiry. Async. */
    edr_status_t (*refresh_token)(edr_auth_token_t    *inout_token,
                                   edr_completion_cb_t  cb,
                                   void                *ctx);

    /* get_current_token — return the active token without network I/O. */
    edr_status_t (*get_current_token)(edr_auth_token_t *out_token);

    /*
     * rotate_certificate — initiate a certificate rotation.
     * Generates a new key pair locally, submits CSR to controller,
     * installs the signed certificate on completion.
     */
    edr_status_t (*rotate_certificate)(edr_completion_cb_t cb, void *ctx);

    /* revoke — invalidate the current session immediately. */
    edr_status_t (*revoke)(edr_completion_cb_t cb, void *ctx);

    /* sign_payload — HMAC or sign a buffer with the agent's private key.
     * Used by comm transport to sign outbound messages. */
    edr_status_t (*sign_payload)(const edr_buf_t *in,
                                  edr_buf_t       *out_sig);

    /* verify_payload — verify a signature from the controller. */
    edr_status_t (*verify_payload)(const edr_buf_t *payload,
                                    const edr_buf_t *sig,
                                    int             *out_valid);
};

#endif /* EDR_INTERFACES_H */
