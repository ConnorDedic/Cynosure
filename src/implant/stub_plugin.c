/*
 * stub_plugin.c — Minimal plugin skeleton that satisfies the full plugin ABI.
 *
 * Copy this file to start a new plugin. Implement the interfaces you need;
 * leave the rest returning EDR_ERR_NOT_IMPL and the dispatcher will fall
 * through to the next registered plugin for that capability.
 *
 * Build as a shared library:
 *   gcc -shared -fPIC -o stub_plugin.so stub_plugin.c -I../../include
 */

#include "edr_plugin.h"
#include "edr_interfaces.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Plugin state — file-scope, no heap needed for a minimal plugin.
 * -------------------------------------------------------------------------*/
static const edr_plugin_services_t *g_svc     = NULL;
static const edr_agent_identity_t  *g_identity = NULL;

/* -------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/
static edr_status_t stub_init(const edr_plugin_services_t *svc,
                               const edr_agent_identity_t  *identity)
{
    g_svc      = svc;
    g_identity = identity;
    if (svc && svc->log)
        svc->log(EDR_LOG_INFO, "stub_plugin",
                 "Initialized for agent '%s'", identity->agent_id);
    return EDR_OK;
}

static edr_status_t stub_shutdown(void)
{
    if (g_svc && g_svc->log)
        g_svc->log(EDR_LOG_INFO, "stub_plugin", "Shutdown");
    g_svc = NULL;
    return EDR_OK;
}

/* -------------------------------------------------------------------------
 * ICommTransport — stub implementation
 * -------------------------------------------------------------------------*/
static edr_status_t comm_connect(const edr_endpoint_t *ep,
                                  edr_completion_cb_t cb, void *ctx)
{
    if (g_svc) g_svc->log(EDR_LOG_INFO, "stub_comm",
                           "connect() → %s:%u", ep->url, ep->port);
    /* TODO: implement actual transport (HTTPS, MQTT, gRPC, …) */
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_disconnect(edr_completion_cb_t cb, void *ctx)
{
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_send(const edr_message_t *msg,
                               edr_completion_cb_t cb, void *ctx)
{
    (void)msg;
    /* TODO: serialize and send msg */
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t comm_set_recv(edr_recv_cb_t handler, void *ctx)
{
    (void)handler; (void)ctx;
    /* TODO: store handler and fire it when inbound messages arrive */
    return EDR_OK;
}

static edr_status_t comm_heartbeat(edr_completion_cb_t cb, void *ctx)
{
    if (g_svc) g_svc->log(EDR_LOG_DEBUG, "stub_comm", "heartbeat");
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static int comm_is_connected(void) { return 1; }

static const edr_iface_comm_t s_comm = {
    .connect         = comm_connect,
    .disconnect      = comm_disconnect,
    .send            = comm_send,
    .set_recv_handler = comm_set_recv,
    .heartbeat       = comm_heartbeat,
    .is_connected    = comm_is_connected,
};

/* -------------------------------------------------------------------------
 * IFileOps — stub (returns EDR_ERR_NOT_IMPL for all; remove to implement)
 * -------------------------------------------------------------------------*/
static const edr_iface_file_ops_t s_file_ops = {
    .collect          = NULL,
    .upload           = NULL,
    .download         = NULL,
    .install          = NULL,
    .verify_integrity = NULL,
    .delete_file      = NULL,
};

/* -------------------------------------------------------------------------
 * IScanEngine — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_scan_t s_scan = {
    .scan       = NULL,
    .cancel     = NULL,
    .load_rules = NULL,
};

/* -------------------------------------------------------------------------
 * IEventStream — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_event_t s_event = {
    .subscribe      = NULL,
    .unsubscribe    = NULL,
    .emit           = NULL,
    .flush          = NULL,
    .set_batch_size = NULL,
};

/* -------------------------------------------------------------------------
 * IRemediation — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_remediation_t s_remediation = {
    .kill_process      = NULL,
    .quarantine_file   = NULL,
    .restore_file      = NULL,
    .delete_quarantined = NULL,
    .block_network     = NULL,
    .unblock_network   = NULL,
    .isolate_host      = NULL,
    .unisolate_host    = NULL,
};

/* -------------------------------------------------------------------------
 * IConfigSync — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_config_t s_config = {
    .pull_policy          = NULL,
    .push_state           = NULL,
    .set_policy_handler   = NULL,
    .apply_policy         = NULL,
    .get_current_revision = NULL,
};

/* -------------------------------------------------------------------------
 * IHealthReport — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_health_t s_health = {
    .collect              = NULL,
    .self_integrity_check = NULL,
    .report               = NULL,
    .set_resource_limits  = NULL,
};

/* -------------------------------------------------------------------------
 * IAuthProvider — stub
 * -------------------------------------------------------------------------*/
static const edr_iface_auth_t s_auth = {
    .authenticate      = NULL,
    .refresh_token     = NULL,
    .get_current_token = NULL,
    .rotate_certificate = NULL,
    .revoke            = NULL,
    .sign_payload      = NULL,
    .verify_payload    = NULL,
};

/* -------------------------------------------------------------------------
 * Interface accessors
 * -------------------------------------------------------------------------*/
static const edr_iface_comm_t        *get_comm(void)        { return &s_comm; }
static const edr_iface_file_ops_t    *get_file_ops(void)    { return &s_file_ops; }
static const edr_iface_scan_t        *get_scan(void)        { return &s_scan; }
static const edr_iface_event_t       *get_event(void)       { return &s_event; }
static const edr_iface_remediation_t *get_remediation(void) { return &s_remediation; }
static const edr_iface_config_t      *get_config(void)      { return &s_config; }
static const edr_iface_health_t      *get_health(void)      { return &s_health; }
static const edr_iface_auth_t        *get_auth(void)        { return &s_auth; }

/* -------------------------------------------------------------------------
 * Plugin entry point — the one exported symbol
 * -------------------------------------------------------------------------*/
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out)
{
    if (!out) return EDR_ERR_INVALID_ARG;

    out->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    strncpy(out->name,   "stub_plugin",  sizeof(out->name)   - 1);
    strncpy(out->vendor, "YourVendor",   sizeof(out->vendor) - 1);

    out->plugin_version.major = 0;
    out->plugin_version.minor = 1;
    out->plugin_version.patch = 0;

    /* Advertise which capabilities this plugin handles. */
    out->capabilities = EDR_CAP_COMM_TRANSPORT; /* expand as implemented */
    out->priority     = 100; /* lower = higher priority */

    out->init     = stub_init;
    out->shutdown = stub_shutdown;

    out->get_comm        = get_comm;
    out->get_file_ops    = get_file_ops;
    out->get_scan        = get_scan;
    out->get_event       = get_event;
    out->get_remediation = get_remediation;
    out->get_config      = get_config;
    out->get_health      = get_health;
    out->get_auth        = get_auth;

    return EDR_OK;
}
