/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-09 21:31:02
 * @LastEditTime: 2020-05-21 01:09:29
 * @Description: the code belongs to jiejie, please keep the author information and source code according to the license.
 */
#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "mqtt_defconfig.h"

#define     NETWORK_CHANNEL_TCP     0
#define     NETWORK_CHANNEL_TLS     1

typedef struct network {
    const char                  *host;
    const char                  *port;
    int                         socket;
#ifdef KAWAII_MQTT_NETWORK_TYPE_TLS
    int                         channel;        /* tcp or tls */
    const char                  *ca_crt;
    unsigned int                ca_crt_len;
    const char                  *client_cert;
    unsigned int                client_cert_len;
    const char                  *client_pk;
    unsigned int                client_pk_len;
    unsigned int                timeout_ms;            // SSL handshake timeout in millisecond
    void                        *nettype_tls_params;
#endif
} network_t;

int network_init(network_t *n, const char *host, const char *port, const char *ca, const char *cert, const char *pk);
int network_set_ca(network_t *n, const char *ca);
void network_set_channel(network_t *n, int channel);
int network_set_host_port(network_t* n, char *host, char *port);
int network_read(network_t* n, unsigned char* buf, int len, int timeout);
int network_write(network_t* n, unsigned char* buf, int len, int timeout);
int network_connect(network_t* n);
void network_disconnect(network_t *n);
void network_release(network_t* n);

#endif
