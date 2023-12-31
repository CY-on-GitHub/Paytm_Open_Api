/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include "infra_config.h"
#include "infra_log.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/debug.h"
#include "mbedtls/platform.h"

#include "osi_api.h"
#include "at_api.h"

#define SEND_TIMEOUT_SECONDS (10)
#define URL_TEST       "pay.chinainfosafe.com"
#define TCP_PORT       (6019)

#define SSL_Printf  RTI_LOG

#ifndef CONFIG_MBEDTLS_DEBUG_LEVEL
#define CONFIG_MBEDTLS_DEBUG_LEVEL 0
#endif

typedef struct _TLSDataParams
{
    mbedtls_ssl_context ssl;  /**< mbed TLS control context. */
    mbedtls_net_context fd;   /**< mbed TLS network context. */
    mbedtls_ssl_config conf;  /**< mbed TLS configuration context. */
    mbedtls_x509_crt cacertl; /**< mbed TLS CA certification. */
    mbedtls_x509_crt clicert; /**< mbed TLS Client certification. */
    mbedtls_pk_context pkey;  /**< mbed TLS Client key. */
} TLSDataParams_t, *TLSDataParams_pt;

static unsigned int mbedtls_mem_used = 0;
static unsigned int mbedtls_max_mem_used = 0;

#define MBEDTLS_MEM_INFO_MAGIC 0x12345678

typedef struct
{
    int magic;
    int size;
} mbedtls_mem_info_t;

#if defined(TLS_SAVE_TICKET)

#define TLS_MAX_SESSION_BUF 384
#define KV_SESSION_KEY "TLS_SESSION"

extern int HAL_Kv_Set(const char *key, const void *val, int len, int sync);

extern int HAL_Kv_Get(const char *key, void *val, int *buffer_len);

static mbedtls_ssl_session *saved_session = NULL;

static int ssl_serialize_session(const mbedtls_ssl_session *session,
                                 unsigned char *buf, size_t buf_len,
                                 size_t *olen)
{
    unsigned char *p = buf;
    size_t left = buf_len;

    if (left < sizeof(mbedtls_ssl_session))
    {
        return (MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL);
    }

    memcpy(p, session, sizeof(mbedtls_ssl_session));
    p += sizeof(mbedtls_ssl_session);
    left -= sizeof(mbedtls_ssl_session);
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    if (left < sizeof(mbedtls_ssl_session))
    {
        return (MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL);
    }
    memcpy(p, session->ticket, session->ticket_len);
    p += session->ticket_len;
    left -= session->ticket_len;
#endif

    *olen = p - buf;

    return (0);
}

static int ssl_deserialize_session(mbedtls_ssl_session *session,
                                   const unsigned char *buf, size_t len)
{
    const unsigned char *p = buf;
    const unsigned char *const end = buf + len;

    if (sizeof(mbedtls_ssl_session) > (size_t)(end - p))
    {
        return (MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
    }

    memcpy(session, p, sizeof(mbedtls_ssl_session));
    p += sizeof(mbedtls_ssl_session);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    session->peer_cert = NULL;
#endif

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
    if (session->ticket_len > 0)
    {
        if (session->ticket_len > (size_t)(end - p))
        {
            return (MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
        }
        session->ticket = HAL_Malloc(session->ticket_len);
        if (session->ticket == NULL)
        {
            return (MBEDTLS_ERR_SSL_ALLOC_FAILED);
        }
        memcpy(session->ticket, p, session->ticket_len);
        p += session->ticket_len;
        SSL_Printf("saved ticket len = %d \r\n", (int)session->ticket_len);
    }
#endif

    if (p != end)
    {
        return (MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
    }

    return (0);
}
#endif

static unsigned int _avRandom()
{
    return (((unsigned int)rand() << 16) + rand());
}

static int _ssl_random(void *p_rng, unsigned char *output, size_t output_len)
{
    uint32_t rnglen = output_len;
    uint8_t rngoffset = 0;

    while (rnglen > 0)
    {
        *(output + rngoffset) = (unsigned char)_avRandom();
        rngoffset++;
        rnglen--;
    }
    return 0;
}

static void _ssl_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    ((void)level);
    if (NULL != ctx)
    {
#if 0
        fprintf((FILE *) ctx, "%s:%04d: %s", file, line, str);
        fflush((FILE *) ctx);
#endif
        SSL_Printf("%s\n", str);
    }
}

static int _real_confirm(int verify_result)
{
    SSL_Printf("certificate verification result: 0x%02x\n", verify_result);

#if defined(FORCE_SSL_VERIFY)
    if ((verify_result & MBEDTLS_X509_BADCERT_EXPIRED) != 0)
    {
        SSL_Printf("! fail ! ERROR_CERTIFICATE_EXPIRED\n");
        return -1;
    }

    if ((verify_result & MBEDTLS_X509_BADCERT_REVOKED) != 0)
    {
        SSL_Printf("! fail ! server certificate has been revoked\n");
        return -1;
    }

    if ((verify_result & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0)
    {
        SSL_Printf("! fail ! CN mismatch\n");
        return -1;
    }

    if ((verify_result & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0)
    {
        SSL_Printf("! fail ! self-signed or not signed by a trusted CA\n");
        return -1;
    }
#endif

    return 0;
}

static int _ssl_client_init(mbedtls_ssl_context *ssl,
                            mbedtls_net_context *tcp_fd,
                            mbedtls_ssl_config *conf,
                            mbedtls_x509_crt *crt509_ca, const char *ca_crt, size_t ca_len,
                            mbedtls_x509_crt *crt509_cli, const char *cli_crt, size_t cli_len,
                            mbedtls_pk_context *pk_cli, const char *cli_key, size_t key_len, const char *cli_pwd, size_t pwd_len)
{
    int ret = -1;

    /*
     * 0. Initialize the RNG and the session data
     */
#if defined(MBEDTLS_DEBUG_C)
    mbedtls_debug_set_threshold((int)CONFIG_MBEDTLS_DEBUG_LEVEL);
#endif
    mbedtls_net_init(tcp_fd);
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_x509_crt_init(crt509_ca);

    /*verify_source->trusted_ca_crt==NULL
     * 0. Initialize certificates
     */

    SSL_Printf("Loading the CA root certificate ...\n");
    if (NULL != ca_crt)
    {
        if (0 != (ret = mbedtls_x509_crt_parse(crt509_ca, (const unsigned char *)ca_crt, ca_len)))
        {
            SSL_Printf(" failed ! x509parse_crt returned -0x%04x\n", -ret);
            return ret;
        }
    }
    SSL_Printf(" ok (%d skipped)\n", ret);

    /* Setup Client Cert/Key */
#if defined(MBEDTLS_X509_CRT_PARSE_C)
#if defined(MBEDTLS_CERTS_C)
    mbedtls_x509_crt_init(crt509_cli);
    mbedtls_pk_init(pk_cli);
#endif
    if (cli_crt != NULL && cli_key != NULL)
    {
#if defined(MBEDTLS_CERTS_C)
        SSL_Printf("start prepare client cert .\n");
        ret = mbedtls_x509_crt_parse(crt509_cli, (const unsigned char *)cli_crt, cli_len);
#else
        {
            ret = 1;
            SSL_Printf("MBEDTLS_CERTS_C not defined.\n");
        }
#endif
        if (ret != 0)
        {
            SSL_Printf(" failed!  mbedtls_x509_crt_parse returned -0x%x\n", -ret);
            return ret;
        }

#if defined(MBEDTLS_CERTS_C)
        SSL_Printf("start mbedtls_pk_parse_key[%s]\n", cli_pwd);
        ret = mbedtls_pk_parse_key(pk_cli, (const unsigned char *)cli_key, key_len, (const unsigned char *)cli_pwd, pwd_len);
#else
        {
            ret = 1;
            SSL_Printf("MBEDTLS_CERTS_C not defined.\n");
        }
#endif

        if (ret != 0)
        {
            SSL_Printf(" failed\n  !  mbedtls_pk_parse_key returned -0x%x\n", -ret);
            return ret;
        }
    }
#endif /* MBEDTLS_X509_CRT_PARSE_C */

    return 0;
}

void *_SSLCalloc_wrapper(size_t n, size_t size)
{
    unsigned char *buf = NULL;
    mbedtls_mem_info_t *mem_info = NULL;

    if (n == 0 || size == 0)
    {
        return NULL;
    }

    buf = (unsigned char *)(malloc(n * size + sizeof(mbedtls_mem_info_t)));
    if (NULL == buf)
    {
        return NULL;
    }
    else
    {
        memset(buf, 0, n * size + sizeof(mbedtls_mem_info_t));
    }

    mem_info = (mbedtls_mem_info_t *)buf;
    mem_info->magic = MBEDTLS_MEM_INFO_MAGIC;
    mem_info->size = n * size;
    buf += sizeof(mbedtls_mem_info_t);

    mbedtls_mem_used += mem_info->size;
    if (mbedtls_mem_used > mbedtls_max_mem_used)
    {
        mbedtls_max_mem_used = mbedtls_mem_used;
    }

    /* SSL_Printf("INFO -- mbedtls malloc: %p %d  total used: %d  max used: %d\r\n",
                       buf, (int)size, mbedtls_mem_used, mbedtls_max_mem_used); */

    return buf;
}

void _SSLFree_wrapper(void *ptr)
{
    mbedtls_mem_info_t *mem_info = NULL;
    if (NULL == ptr)
    {
        return;
    }

    mem_info = (mbedtls_mem_info_t *)((unsigned char *)ptr - sizeof(mbedtls_mem_info_t));
    if (mem_info->magic != MBEDTLS_MEM_INFO_MAGIC)
    {
        SSL_Printf("Warning - invalid mem info magic: 0x%x\r\n", mem_info->magic);
        return;
    }

    mbedtls_mem_used -= mem_info->size;
    /* SSL_Printf("INFO mbedtls free: %p %d  total used: %d  max used: %d\r\n",
                       ptr, mem_info->size, mbedtls_mem_used, mbedtls_max_mem_used);*/

   free(mem_info);
}

/**
 * @brief This function connects to the specific SSL server with TLS, and returns a value that indicates whether the connection is create successfully or not. Call #NewNetwork() to initialize network structure before calling this function.
 * @param[in] n is the the network structure pointer.
 * @param[in] addr is the Server Host name or IP address.
 * @param[in] port is the Server Port.
 * @param[in] ca_crt is the Server's CA certification.
 * @param[in] ca_crt_len is the length of Server's CA certification.
 * @param[in] client_crt is the client certification.
 * @param[in] client_crt_len is the length of client certification.
 * @param[in] client_key is the client key.
 * @param[in] client_key_len is the length of client key.
 * @param[in] client_pwd is the password of client key.
 * @param[in] client_pwd_len is the length of client key's password.
 * @sa #NewNetwork();
 * @return If the return value is 0, the connection is created successfully. If the return value is -1, then calling lwIP #socket() has failed. If the return value is -2, then calling lwIP #connect() has failed. Any other value indicates that calling lwIP #getaddrinfo() has failed.
 */

static int _TLSConnectNetwork(TLSDataParams_t *pTlsData, const char *addr, const char *port,
                              const char *ca_crt, size_t ca_crt_len,
                              const char *client_crt, size_t client_crt_len,
                              const char *client_key, size_t client_key_len,
                              const char *client_pwd, size_t client_pwd_len)
{
    int ret = -1;
    /*
     * 0. Init
     */
    if (0 != (ret = _ssl_client_init(&(pTlsData->ssl), &(pTlsData->fd), &(pTlsData->conf),
                                     &(pTlsData->cacertl), ca_crt, ca_crt_len,
                                     &(pTlsData->clicert), client_crt, client_crt_len,
                                     &(pTlsData->pkey), client_key, client_key_len, client_pwd, client_pwd_len)))
    {
        SSL_Printf(" failed ! ssl_client_init returned -0x%04x\n", -ret);
        return ret;
    }

    /*
     * 1. Start the connection
     */
    SSL_Printf("Connecting to /%s/%s...\n", addr, port);
#if defined(_PLATFORM_IS_LINUX_)
    if (0 != (ret = mbedtls_net_connect_timeout(&(pTlsData->fd), addr, port, MBEDTLS_NET_PROTO_TCP,
                                                SEND_TIMEOUT_SECONDS)))
    {
        SSL_Printf(" failed ! net_connect returned -0x%04x\n", -ret);
        return ret;
    }
#else
    if (0 != (ret = mbedtls_net_connect(&(pTlsData->fd), addr, port, MBEDTLS_NET_PROTO_TCP)))
    {
        SSL_Printf(" failed ! net_connect returned -0x%04x\n", -ret);
        return ret;
    }
#endif
    SSL_Printf(" ok\n");

    /*
     * 2. Setup stuff
     */
    SSL_Printf("  . Setting up the SSL/TLS structure...\n");
    if ((ret = mbedtls_ssl_config_defaults(&(pTlsData->conf), MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        SSL_Printf(" failed! mbedtls_ssl_config_defaults returned %d\n", ret);
        return ret;
    }

    // mbedtls_ssl_conf_max_version(&pTlsData->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    // mbedtls_ssl_conf_min_version(&pTlsData->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

    SSL_Printf(" ok\n");

    /* OPTIONAL is not optimal for security, but makes interop easier in this simplified example */
    if (ca_crt != NULL)
    {
#if defined(FORCE_SSL_VERIFY)
        mbedtls_ssl_conf_authmode(&(pTlsData->conf), MBEDTLS_SSL_VERIFY_REQUIRED);
#else
        mbedtls_ssl_conf_authmode(&(pTlsData->conf), MBEDTLS_SSL_VERIFY_OPTIONAL);
#endif
    }
    else
    {
        mbedtls_ssl_conf_authmode(&(pTlsData->conf), MBEDTLS_SSL_VERIFY_NONE);
    }

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_ssl_conf_ca_chain(&(pTlsData->conf), &(pTlsData->cacertl), NULL);

    if ((ret = mbedtls_ssl_conf_own_cert(&(pTlsData->conf), &(pTlsData->clicert), &(pTlsData->pkey))) != 0)
    {
        SSL_Printf(" failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n", ret);
        return ret;
    }
#endif
    mbedtls_ssl_conf_rng(&(pTlsData->conf), _ssl_random, NULL);
    mbedtls_ssl_conf_dbg(&(pTlsData->conf), _ssl_debug, NULL);
    mbedtls_ssl_conf_dbg(&(pTlsData->conf), _ssl_debug, stdout);

    if ((ret = mbedtls_ssl_setup(&(pTlsData->ssl), &(pTlsData->conf))) != 0)
    {
        SSL_Printf("failed! mbedtls_ssl_setup returned %d\n", ret);
        return ret;
    }
#if defined(ON_PRE) || defined(ON_DAILY)
    SSL_Printf("SKIPPING mbedtls_ssl_set_hostname() when ON_PRE or ON_DAILY defined!\n");
#else
    mbedtls_ssl_set_hostname(&(pTlsData->ssl), addr);
#endif
    mbedtls_ssl_set_bio(&(pTlsData->ssl), &(pTlsData->fd), mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

#if defined(TLS_SAVE_TICKET)
    if (NULL == saved_session)
    {
        do
        {
            int len = TLS_MAX_SESSION_BUF;
            unsigned char *save_buf = HAL_Malloc(TLS_MAX_SESSION_BUF);
            if (save_buf == NULL)
            {
                SSL_Printf(" malloc failed\r\n");
                break;
            }

            saved_session = HAL_Malloc(sizeof(mbedtls_ssl_session));

            if (saved_session == NULL)
            {
                SSL_Printf(" malloc failed\r\n");
                HAL_Free(save_buf);
                save_buf = NULL;
                break;
            }

            memset(save_buf, 0x00, TLS_MAX_SESSION_BUF);
            memset(saved_session, 0x00, sizeof(mbedtls_ssl_session));

            ret = HAL_Kv_Get(KV_SESSION_KEY, save_buf, &len);

            if (ret != 0 || len == 0)
            {
                SSL_Printf(" kv get failed len=%d,ret = %d\r\n", len, ret);
                HAL_Free(saved_session);
                HAL_Free(save_buf);
                save_buf = NULL;
                saved_session = NULL;
                break;
            }
            ret = ssl_deserialize_session(saved_session, save_buf, len);
            if (ret < 0)
            {
                SSL_Printf("ssl_deserialize_session err,ret = %d\r\n", ret);
                HAL_Free(saved_session);
                HAL_Free(save_buf);
                save_buf = NULL;
                saved_session = NULL;
                break;
            }
            HAL_Free(save_buf);
        } while (0);
    }

    if (NULL != saved_session)
    {
        mbedtls_ssl_set_session(&(pTlsData->ssl), saved_session);
        SSL_Printf("use saved session!!\r\n");
    }
#endif
    /*
      * 4. Handshake
      */
    mbedtls_ssl_conf_read_timeout(&(pTlsData->conf), 10000);
    SSL_Printf("Performing the SSL/TLS handshake...\n");

    while ((ret = mbedtls_ssl_handshake(&(pTlsData->ssl))) != 0)
    {
        if ((ret != MBEDTLS_ERR_SSL_WANT_READ) && (ret != MBEDTLS_ERR_SSL_WANT_WRITE))
        {
            SSL_Printf("failed  ! mbedtls_ssl_handshake returned -0x%04x\n", -ret);
            return ret;
        }
    }
    SSL_Printf(" ok\n");

#if defined(TLS_SAVE_TICKET)
    if (NULL == saved_session)
    {
        do
        {
            size_t real_session_len = 0;
            unsigned char *save_buf = HAL_Malloc(TLS_MAX_SESSION_BUF); //for test
            if (save_buf == NULL)
            {
                break;
            }

            saved_session = HAL_Malloc(sizeof(mbedtls_ssl_session));
            if (NULL == saved_session)
            {
                HAL_Free(save_buf);
                break;
            }
            memset(save_buf, 0x00, sizeof(TLS_MAX_SESSION_BUF));
            memset(saved_session, 0x00, sizeof(mbedtls_ssl_session));

            ret = mbedtls_ssl_get_session(&(pTlsData->ssl), saved_session);
            if (ret != 0)
            {
                HAL_Free(save_buf);
                HAL_Free(saved_session);
                saved_session = NULL;
                break;
            }
            ret = ssl_serialize_session(saved_session, save_buf, TLS_MAX_SESSION_BUF, &real_session_len);
            SSL_Printf("mbedtls_ssl_get_session_session return 0x%04x real_len=%d\r\n", ret, (int)real_session_len);
            if (ret == 0)
            {
                HAL_Kv_Set(KV_SESSION_KEY, (void *)save_buf, real_session_len, 1);
            }
            HAL_Free(save_buf);
        } while (0);
    }
#endif

    /*
     * 5. Verify the server certificate
     */
    SSL_Printf("  . Verifying peer X.509 certificate..\n");
    if (0 != (ret = _real_confirm(mbedtls_ssl_get_verify_result(&(pTlsData->ssl)))))
    {
        SSL_Printf(" failed  ! verify result not confirmed.\n");
        return ret;
    }
    /* n->my_socket = (int)((n->tlsdataparams.fd).fd); */
    /* WRITE_IOT_DEBUG_LOG("my_socket=%d", n->my_socket); */

    return 0;
}

static int _network_ssl_read(TLSDataParams_t *pTlsData, char *buffer, int len, int timeout_ms)
{
    uint32_t readLen = 0;
    static int net_status = 0;
    int ret = -1;
    char err_str[33];

    mbedtls_ssl_conf_read_timeout(&(pTlsData->conf), timeout_ms);
    while (readLen < len)
    {
        ret = mbedtls_ssl_read(&(pTlsData->ssl), (unsigned char *)(buffer + readLen), (len - readLen));
        if (ret > 0)
        {
            readLen += ret;
            net_status = 0;
        }
        else if (ret == 0)
        {
            /* if ret is 0 and net_status is -2, indicate the connection is closed during last call */
            return (net_status == -2) ? net_status : readLen;
        }
        else
        {
            if (MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY == ret)
            {
                // mbedtls_strerror(ret, err_str, sizeof(err_str));
                SSL_Printf("ssl recv error: code = %d, err_str = '%s'\n", ret, err_str);
                net_status = -2; /* connection is closed */
                break;
            }
            else if ((MBEDTLS_ERR_SSL_TIMEOUT == ret) || (MBEDTLS_ERR_SSL_CONN_EOF == ret) || (MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED == ret) || (MBEDTLS_ERR_SSL_NON_FATAL == ret))
            {
                /* read already complete */
                /* if call mbedtls_ssl_read again, it will return 0 (means EOF) */

                return readLen;
            }
            else
            {
                // mbedtls_strerror(ret, err_str, sizeof(err_str));
                SSL_Printf("ssl recv error: code = %d, err_str = '%s'\n", ret, err_str);
                net_status = -1;
                return -1; /* Connection error */
            }
        }
    }

    return (readLen > 0) ? readLen : net_status;
}

static int _network_ssl_write(TLSDataParams_t *pTlsData, const char *buffer, int len, int timeout_ms)
{
#if defined(_PLATFORM_IS_LINUX_)
    int32_t res = 0;
    int32_t write_bytes = 0;
    uint64_t timestart_ms = 0, timenow_ms = 0;
    struct timeval timestart, timenow, timeout;

    if (pTlsData == NULL)
    {
        return -1;
    }

    /* timeout */
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    /* Start Time */
    gettimeofday(&timestart, NULL);
    timestart_ms = timestart.tv_sec * 1000 + timestart.tv_usec / 1000;
    timenow_ms = timestart_ms;

    res = setsockopt(pTlsData->fd.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (res < 0)
    {
        return -1;
    }

    do
    {
        gettimeofday(&timenow, NULL);
        timenow_ms = timenow.tv_sec * 1000 + timenow.tv_usec / 1000;

        if (timenow_ms - timestart_ms >= timenow_ms ||
            timeout_ms - (timenow_ms - timestart_ms) > timeout_ms)
        {
            break;
        }

        res = mbedtls_ssl_write(&(pTlsData->ssl), (unsigned char *)buffer + write_bytes, len - write_bytes);
        if (res < 0)
        {
            if (res != MBEDTLS_ERR_SSL_WANT_READ &&
                res != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                if (write_bytes == 0)
                {
                    return -1;
                }
                break;
            }
        }
        else if (res == 0)
        {
            break;
        }
        else
        {
            write_bytes += res;
        }
    } while (((timenow_ms - timestart_ms) < timeout_ms) && (write_bytes < len));

    return write_bytes;
#else
    uint32_t writtenLen = 0;
    int ret = -1;

    if (pTlsData == NULL)
    {
        return -1;
    }

    while (writtenLen < len)
    {
        ret = mbedtls_ssl_write(&(pTlsData->ssl), (unsigned char *)(buffer + writtenLen), (len - writtenLen));
        if (ret > 0)
        {
            writtenLen += ret;
            continue;
        }
        else if (ret == 0)
        {
            SSL_Printf("ssl write timeout\n");
            return 0;
        }
        else
        {
            char err_str[33];
            // mbedtls_strerror(ret, err_str, sizeof(err_str));
            SSL_Printf("ssl write fail, code=%d, str=%s\n", ret, err_str);
            return -1; /* Connnection error */
        }
    }

    return writtenLen;
#endif
}

static void _network_ssl_disconnect(TLSDataParams_t *pTlsData)
{
    mbedtls_ssl_close_notify(&(pTlsData->ssl));
    mbedtls_net_free(&(pTlsData->fd));
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_x509_crt_free(&(pTlsData->cacertl));
    if ((pTlsData->pkey).pk_info != NULL)
    {
        SSL_Printf("need release client crt&key\n");
#if defined(MBEDTLS_CERTS_C)
        mbedtls_x509_crt_free(&(pTlsData->clicert));
        mbedtls_pk_free(&(pTlsData->pkey));
#endif
    }
#endif
    mbedtls_ssl_free(&(pTlsData->ssl));
    mbedtls_ssl_config_free(&(pTlsData->conf));
    SSL_Printf("ssl_disconnect\n");
}

int HAL_SSL_Read(uintptr_t handle, char *buf, int len, int timeout_ms)
{
    return _network_ssl_read((TLSDataParams_t *)handle, buf, len, timeout_ms);
    ;
}

int HAL_SSL_Write(uintptr_t handle, const char *buf, int len, int timeout_ms)
{
    return _network_ssl_write((TLSDataParams_t *)handle, buf, len, timeout_ms);
}

int32_t HAL_SSL_Destroy(uintptr_t handle)
{
    if ((uintptr_t)NULL == handle)
    {
        SSL_Printf("handle is NULL\n");
        return 0;
    }

    _network_ssl_disconnect((TLSDataParams_t *)handle);
    free((void *)handle);
    return 0;
}

uintptr_t HAL_SSL_Establish(const char *host,
                            uint16_t port,
                            const char *ca_crt,
                            uint32_t ca_crt_len)
{
    char port_str[6];
    const char *alter = host;
    TLSDataParams_pt pTlsData;

    if (host == NULL)
    {
        SSL_Printf("input params are NULL, abort\n");
        return 0;
    }

    if (!strlen(host) || (strlen(host) < 8))
    {
        SSL_Printf("invalid host: '%s'(len=%d), abort\n", host, (int)strlen(host));
        return 0;
    }

    pTlsData = malloc(sizeof(TLSDataParams_t));
    if (NULL == pTlsData)
    {
        return (uintptr_t)NULL;
    }
    memset(pTlsData, 0x0, sizeof(TLSDataParams_t));

    sprintf(port_str, "%u", port);

#if defined(ON_PRE)
    if (!strcmp(GUIDER_ONLINE_HOSTNAME, host))
    {
        SSL_Printf("ALTERING '%s' to '%s' since ON_PRE defined!\n", host, GUIDER_PRE_ADDRESS);
        alter = GUIDER_PRE_ADDRESS;
    }
#endif

    // mbedtls_platform_set_calloc_free(_SSLCalloc_wrapper, _SSLFree_wrapper);

    if (0 != _TLSConnectNetwork(pTlsData, alter, port_str, ca_crt, ca_crt_len, NULL, 0, NULL, 0, NULL, 0))
    {
        _network_ssl_disconnect(pTlsData);
        free((void *)pTlsData);
        return (uintptr_t)NULL;
    }

    return (uintptr_t)pTlsData;
}

static int vatResName(const char *rsp, const char *name)
{
    if(rsp == NULL || name == NULL){
        return -1;
    }

    // int ret = strncmp(rsp, name, strlen(name));
    if(strncmp(rsp, name, strlen(name)) == 0)
    {
        return strlen(name);
    }
    return -1;
}

static bool prvUpdateCREG2(){
    char resp[128] = {0};
    char *cmd = "AT+CEREG?\r\n";
    int n;
    int stat;
    char lac[5] = {0};
    char ci[9] = {0};
    int act;
    int ret = atCmdSendWaitResp(cmd, 1, "+CEREG", 1, "ERROR", resp, sizeof(resp));
    if(ret == 0){
        int pos = vatResName(resp, "+CEREG");
        RTI_LOG("pos:%d",pos);
        if(pos >= 0)
        {
            ret = sscanf(resp + pos, ": %d,%d,\"%4s\",\"%8s\",%d\r\n", &n, &stat, lac, ci, &act);
            if(ret == 5)
            {
                RTI_LOG("n:%d state:%d lac:%s,ci:%s",n,stat,lac,ci);
                
                if(stat == 1 || stat == 5){
                    atCmdSendWaitResp("AT+QIACT=1\r\n", 5, NULL, 1, "ERROR", NULL, 0);
                    return true;
                }else{
                    return false;
                }
            }
        }
        return false;
    }else{
        RTI_LOG("AT CEREG Error");
    }
    return false;
}

static void activeNetwork(void){
    atCmdSendWaitResp("AT+CEREG=2\r\n", 1, NULL, 1, NULL, NULL, 0);
    osiThreadSleep(10);
    atCmdSendWaitResp("AT*DIALMODE=0\r\n", 1, NULL, 1, NULL, NULL, 0);
    osiThreadSleep(10);
    while (1)
    {
        if(prvUpdateCREG2()){
            break;
        }else{
            osiThreadSleep(500);
        }
    }
}


static void prvSslThreadEntry(void *param)
{
    int tcnt = 0;
    char sendBuf[128] = {0};
    char recvBuf[128] = {0};
    int ret = 0;
    int32_t slen = 0;
    int32_t rlen = 0;
    uintptr_t fd;
    
    activeNetwork();

    while (1)    
    {
        while (1)
        {
            fd = HAL_SSL_Establish(URL_TEST, TCP_PORT, NULL, 0);
            if(fd == 0){
                RTI_LOG("TCP connect server:%s:%d FALSE",URL_TEST, TCP_PORT);
                osiThreadSleep(1000);
            }else{
                RTI_LOG("TCP connect server:%s:%d SUCCESS",URL_TEST, TCP_PORT);
                break;
            }
        }

        // srand(OSIGetTicks());
        strcpy(sendBuf, "Hello Chinainfosafe");
        tcnt = 0;
        while (tcnt++ < 10)
        {
            RTI_LOG("TCP SEND:%s slen addr:%x", sendBuf,&slen);
            slen = HAL_SSL_Write(fd, sendBuf ,strlen(sendBuf),1000);
            RTI_LOG("HAL_TCP_Write slen:%d ", slen);

            if(slen <= 0){
                RTI_LOG("send buff error");
                HAL_SSL_Destroy(fd);
                goto _EXIT;
            }
            memset(recvBuf, 0, sizeof(recvBuf));
            RTI_LOG("TCP read  rlen addr:%x ", &rlen);
            rlen = HAL_SSL_Read(fd, recvBuf, sizeof(recvBuf), 4000);
            RTI_LOG("TCP read success ret:%d rlen:%d ", ret, rlen);
            if( rlen <= 0){
                RTI_LOG("recv from socekt size error rlen:%d",rlen);
                HAL_SSL_Destroy(fd);
                goto _EXIT;
            }
            RTI_LOG("TCP read :%s ", recvBuf);

        }
        HAL_SSL_Destroy(fd);
        fd = -1;
    }
_EXIT:
    osiThreadExit();
}

int appimg_enter(void *param)
{
    RTI_LOG("application image enter, param 0x%x", param);
    osiThreadCreate("tcp_example", prvSslThreadEntry, NULL, 18, 10*1024);
    return 0;
}

void appimg_exit(void)
{
    RTI_LOG("application image exit");
}