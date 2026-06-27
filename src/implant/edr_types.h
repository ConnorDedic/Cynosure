#ifndef EDR_TYPES_H
#define EDR_TYPES_H

/*
 * edr_types.h — Shared primitive types for the EDR agent dispatcher.
 * All interfaces and plugins include this header. No business logic here.
 */

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Versioning
 * -------------------------------------------------------------------------*/
#define EDR_INTERFACE_VERSION_MAJOR  1
#define EDR_INTERFACE_VERSION_MINOR  0
#define EDR_INTERFACE_VERSION_PATCH  0

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} edr_version_t;

/* -------------------------------------------------------------------------
 * Return codes — every interface function returns one of these.
 * -------------------------------------------------------------------------*/
typedef enum {
    EDR_OK                  =  0,   /* Success */
    EDR_ERR_GENERIC         = -1,   /* Unspecified error */
    EDR_ERR_NOT_IMPL        = -2,   /* Plugin does not implement this fn */
    EDR_ERR_INVALID_ARG     = -3,   /* Bad argument passed */
    EDR_ERR_NOT_INIT        = -4,   /* Plugin not yet initialized */
    EDR_ERR_TIMEOUT         = -5,   /* Operation timed out */
    EDR_ERR_AUTH_FAIL       = -6,   /* Authentication/authorization failure */
    EDR_ERR_IO              = -7,   /* I/O or filesystem error */
    EDR_ERR_NETWORK         = -8,   /* Network-level failure */
    EDR_ERR_NO_MEMORY       = -9,   /* Allocation failure */
    EDR_ERR_BUSY            = -10,  /* Resource busy or rate-limited */
    EDR_ERR_TAMPERED        = -11,  /* Integrity check failed */
    EDR_ERR_UNSUPPORTED     = -12,  /* Platform/config not supported */
    EDR_ERR_QUOTA           = -13,  /* Upload/storage quota exceeded */
    EDR_ERR_PLUGIN_MISMATCH = -14,  /* Plugin ABI version incompatible */
} edr_status_t;

/* -------------------------------------------------------------------------
 * Plugin capability flags — OR these together in the capability manifest.
 * -------------------------------------------------------------------------*/
#define EDR_CAP_COMM_TRANSPORT  (1u << 0)
#define EDR_CAP_FILE_OPS        (1u << 1)
#define EDR_CAP_SCAN_ENGINE     (1u << 2)
#define EDR_CAP_EVENT_STREAM    (1u << 3)
#define EDR_CAP_REMEDIATION     (1u << 4)
#define EDR_CAP_CONFIG_SYNC     (1u << 5)
#define EDR_CAP_HEALTH_REPORT   (1u << 6)
#define EDR_CAP_AUTH_PROVIDER   (1u << 7)

/* -------------------------------------------------------------------------
 * Async completion callback.
 * ctx    — caller-supplied context pointer passed through unchanged.
 * status — result of the async operation.
 * data   — optional result payload (may be NULL); ownership per-interface.
 * -------------------------------------------------------------------------*/
typedef void (*edr_completion_cb_t)(void *ctx, edr_status_t status, void *data);

/* -------------------------------------------------------------------------
 * Generic byte buffer
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t *data;
    size_t   len;
} edr_buf_t;

/* -------------------------------------------------------------------------
 * Agent / endpoint identity
 * -------------------------------------------------------------------------*/
#define EDR_AGENT_ID_LEN   64
#define EDR_HOSTNAME_LEN  256

typedef struct {
    char agent_id[EDR_AGENT_ID_LEN];   /* UUID string */
    char hostname[EDR_HOSTNAME_LEN];
    uint32_t platform;                  /* EDR_PLATFORM_* */
    edr_version_t agent_version;
} edr_agent_identity_t;

#define EDR_PLATFORM_WINDOWS  1
#define EDR_PLATFORM_LINUX    2
#define EDR_PLATFORM_MACOS    3

/* -------------------------------------------------------------------------
 * Log levels (used by dispatcher logger, available to plugins via callback)
 * -------------------------------------------------------------------------*/
typedef enum {
    EDR_LOG_DEBUG = 0,
    EDR_LOG_INFO,
    EDR_LOG_WARN,
    EDR_LOG_ERROR,
    EDR_LOG_FATAL,
} edr_log_level_t;

typedef void (*edr_log_fn_t)(edr_log_level_t level, const char *component,
                              const char *fmt, ...);

#endif /* EDR_TYPES_H */
