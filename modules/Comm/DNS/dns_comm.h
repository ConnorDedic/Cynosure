/*
 * DNS Communication Module - Header
 */

#ifndef DNS_COMM_H
#define DNS_COMM_H

#include <stddef.h>

/* Initialize DNS communication */
int dns_comm_init(const char *c2_domain, const char *server_ip);

/* Send data via DNS */
int dns_send_data(const unsigned char *data, size_t len);

/* Receive command via DNS */
int dns_recv_command(unsigned char *buffer, size_t buffer_size);

/* Get status string */
const char* dns_comm_status(void);

/* Test DNS module */
int dns_comm_test(void);

#endif /* DNS_COMM_H */
