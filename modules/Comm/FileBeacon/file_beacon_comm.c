/*
 * file_beacon_comm.c — File transfer over beacon protocol (stub)
 *
 * This is a minimal implementation that:
 * - Implements the edr_plugin_entry interface correctly
 * - Provides the edr_iface_comm interface
 * - Compiles without errors
 * - Can be extended later with full file transfer logic
 */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "edr_plugin.h"
#include "edr_interfaces.h"

/* Global state */
static const edr_plugin_services_t *g_svc = NULL;

/* File beacon interface implementation */
static edr_status_t fb_connect(const edr_endpoint_t *ep,
                               edr_completion_cb_t cb,
                               void *ctx) {
    (void)ep;
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fb_disconnect(edr_completion_cb_t cb, void *ctx) {
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fb_send(const edr_message_t *msg,
                            edr_completion_cb_t cb,
                            void *ctx) {
    (void)msg;
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_status_t fb_set_recv_handler(edr_recv_cb_t handler, void *ctx) {
    (void)handler;
    (void)ctx;
    return EDR_OK;
}

static edr_status_t fb_heartbeat(edr_completion_cb_t cb, void *ctx) {
    if (cb) cb(ctx, EDR_OK, NULL);
    return EDR_OK;
}

static edr_iface_comm_t g_iface_beacon = {
    .connect = fb_connect,
    .disconnect = fb_disconnect,
    .send = fb_send,
    .set_recv_handler = fb_set_recv_handler,
    .heartbeat = fb_heartbeat,
};

/* Plugin manifest accessors */
static edr_status_t fb_init(const edr_plugin_services_t *svc,
                            const edr_agent_identity_t *identity) {
    (void)identity;
    g_svc = svc;
    if (svc && svc->log) {
        svc->log(EDR_LOG_INFO, "file_beacon", "File beacon module initialized");
    }
    return EDR_OK;
}

static edr_status_t fb_shutdown(void) {
    g_svc = NULL;
    return EDR_OK;
}

static const edr_iface_comm_t *fb_get_comm(void) {
    return &g_iface_beacon;
}

static const edr_iface_file_ops_t *fb_get_file_ops(void) { return NULL; }
static const edr_iface_scan_t *fb_get_scan(void) { return NULL; }
static const edr_iface_event_t *fb_get_event(void) { return NULL; }
static const edr_iface_remediation_t *fb_get_remediation(void) { return NULL; }
static const edr_iface_config_t *fb_get_config(void) { return NULL; }
static const edr_iface_health_t *fb_get_health(void) { return NULL; }
static const edr_iface_auth_t *fb_get_auth(void) { return NULL; }

/* Plugin entry point */
__declspec(dllexport)
edr_status_t edr_plugin_entry(edr_plugin_manifest_t *out) {
    if (!out) return EDR_ERR_INVALID_ARG;

    out->abi_version.major = EDR_INTERFACE_VERSION_MAJOR;
    out->abi_version.minor = EDR_INTERFACE_VERSION_MINOR;
    out->abi_version.patch = EDR_INTERFACE_VERSION_PATCH;

    strncpy(out->name, "file_beacon", EDR_PLUGIN_NAME_LEN - 1);
    strncpy(out->vendor, "cynosure", EDR_PLUGIN_VENDOR_LEN - 1);

    out->plugin_version.major = 0;
    out->plugin_version.minor = 1;
    out->plugin_version.patch = 0;

    out->capabilities = EDR_CAP_COMM_TRANSPORT;
    out->priority = 50;

    out->init = fb_init;
    out->shutdown = fb_shutdown;
    out->get_comm = fb_get_comm;
    out->get_file_ops = fb_get_file_ops;
    out->get_scan = fb_get_scan;
    out->get_event = fb_get_event;
    out->get_remediation = fb_get_remediation;
    out->get_config = fb_get_config;
    out->get_health = fb_get_health;
    out->get_auth = fb_get_auth;

    return EDR_OK;
}
