#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>

static mbedtls_x509_crt g_ClientCert;
static mbedtls_pk_context g_ClientKey;
static bool g_HasClientTls = false;

static mbedtls_entropy_context g_Entropy;
static mbedtls_ctr_drbg_context g_CtrDrbg;
static bool g_RngInit = false;

static void ensure_rng_init(void) {
    if (!g_RngInit) {
        psa_crypto_init();
        mbedtls_entropy_init(&g_Entropy);
        mbedtls_ctr_drbg_init(&g_CtrDrbg);
        const char* pers = "moonlight_xp_client";
        mbedtls_ctr_drbg_seed(&g_CtrDrbg, mbedtls_entropy_func, &g_Entropy,
                              (const unsigned char*)pers, strlen(pers));
        g_RngInit = true;
    }
}

bool http_client_set_tls_identity(const char* cert_pem, const char* key_pem) {
    ensure_rng_init();

    if (g_HasClientTls) {
        mbedtls_x509_crt_free(&g_ClientCert);
        mbedtls_pk_free(&g_ClientKey);
        g_HasClientTls = false;
    }

    if (!cert_pem || !key_pem) return false;

    mbedtls_x509_crt_init(&g_ClientCert);
    mbedtls_pk_init(&g_ClientKey);

    int r1 = mbedtls_x509_crt_parse(&g_ClientCert, (const unsigned char*)cert_pem, strlen(cert_pem) + 1);
    int r2 = mbedtls_pk_parse_key(&g_ClientKey, (const unsigned char*)key_pem, strlen(key_pem) + 1, NULL, 0,
                                  mbedtls_ctr_drbg_random, &g_CtrDrbg);

    if (r1 != 0 || r2 != 0) {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf), "Failed to parse TLS identity:\nCert error: -0x%04x\nKey error: -0x%04x", -r1, -r2);
        MessageBoxA(NULL, errBuf, "TLS Identity Error", MB_ICONERROR);
        mbedtls_x509_crt_free(&g_ClientCert);
        mbedtls_pk_free(&g_ClientKey);
        return false;
    }

    g_HasClientTls = true;
    return true;
}

void http_response_free(HttpResponse* resp) {
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
        resp->status_code = 0;
    }
}

static bool is_http_response_complete(const char* buf, size_t len, size_t* out_header_len, size_t* out_content_len) {
    if (!buf || len == 0) return false;

    const char* header_end = strstr(buf, "\r\n\r\n");
    size_t hdr_sep_len = 4;
    if (!header_end) {
        header_end = strstr(buf, "\n\n");
        hdr_sep_len = 2;
        if (!header_end) return false;
    }

    size_t header_len = (header_end - buf) + hdr_sep_len;
    if (out_header_len) *out_header_len = header_len;

    // Check for Content-Length
    const char* cl_pos = strstr(buf, "Content-Length:");
    if (!cl_pos) cl_pos = strstr(buf, "content-length:");
    if (!cl_pos) cl_pos = strstr(buf, "CONTENT-LENGTH:");

    if (cl_pos && cl_pos < header_end) {
        cl_pos += 15;
        while (*cl_pos == ' ' || *cl_pos == '\t') cl_pos++;
        size_t content_len = (size_t)strtoul(cl_pos, NULL, 10);
        if (out_content_len) *out_content_len = content_len;

        return (len >= header_len + content_len);
    }

    return false;
}

static bool parse_http_response(const char* raw_data, size_t raw_len, HttpResponse* out_resp) {
    if (!raw_data || raw_len == 0 || !out_resp) return false;

    memset(out_resp, 0, sizeof(HttpResponse));

    const char* space = strchr(raw_data, ' ');
    if (!space) return false;

    out_resp->status_code = atoi(space + 1);

    const char* header_end = strstr(raw_data, "\r\n\r\n");
    if (!header_end) {
        header_end = strstr(raw_data, "\n\n");
        if (!header_end) return false;
        header_end += 2;
    } else {
        header_end += 4;
    }

    size_t header_len = header_end - raw_data;
    size_t body_len = (raw_len > header_len) ? (raw_len - header_len) : 0;

    out_resp->body = (char*)malloc(body_len + 1);
    if (!out_resp->body) return false;

    if (body_len > 0) {
        memcpy(out_resp->body, header_end, body_len);
    }
    out_resp->body[body_len] = '\0';
    out_resp->body_len = body_len;

    return true;
}

static int dummy_verify(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    if (flags) *flags = 0;
    return 0;
}

static bool http_request_raw(const char* host, int port, const char* request, size_t req_len, bool use_https, HttpResponse* out_resp, int timeout_ms) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    size_t total_alloc = 16384;
    size_t total_received = 0;
    char* response_buf = (char*)malloc(total_alloc);
    if (!response_buf) return false;

    if (use_https) {
        ensure_rng_init();

        mbedtls_net_context server_fd;
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config conf;

        mbedtls_net_init(&server_fd);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);

        int ret = mbedtls_net_connect(&server_fd, host, port_str, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "HTTPS TCP connect to %s:%d failed (error -0x%04x).", host, port, -ret);
            MessageBoxA(NULL, msg, "Connection Error", MB_ICONWARNING);
            goto cleanup_ssl;
        }

        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            goto cleanup_ssl;
        }

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_verify(&conf, dummy_verify, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_CtrDrbg);

        // Configure client certificate and CA chain for mTLS
        if (g_HasClientTls) {
            mbedtls_ssl_conf_ca_chain(&conf, &g_ClientCert, NULL);
            mbedtls_ssl_conf_own_cert(&conf, &g_ClientCert, &g_ClientKey);
        }

        if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
            goto cleanup_ssl;
        }

        mbedtls_ssl_set_hs_authmode(&ssl, MBEDTLS_SSL_VERIFY_NONE);

        // Only send SNI hostname if host is a DNS name, not an IP address
        bool is_ip = true;
        for (const char* p = host; *p; p++) {
            if (!isdigit((unsigned char)*p) && *p != '.' && *p != ':') {
                is_ip = false;
                break;
            }
        }
        if (!is_ip) {
            mbedtls_ssl_set_hostname(&ssl, host);
        }

        DWORD tv = (timeout_ms > 0) ? (DWORD)timeout_ms : 10000;
        setsockopt((SOCKET)server_fd.fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt((SOCKET)server_fd.fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char err[128];
                mbedtls_strerror(ret, err, sizeof(err));
                char msg[256];
                snprintf(msg, sizeof(msg), "HTTPS TLS handshake to %s:%d failed:\nError: -0x%04x (%s)", host, port, -ret, err);
                MessageBoxA(NULL, msg, "TLS Handshake Error", MB_ICONWARNING);
                goto cleanup_ssl;
            }
        }

        size_t written = 0;
        while (written < req_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char*)request + written, req_len - written);
            if (ret <= 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    char err[128];
                    mbedtls_strerror(ret, err, sizeof(err));
                    char msg[256];
                    snprintf(msg, sizeof(msg), "HTTPS SSL write failed to %s:%d:\nret=%d (-0x%04x: %s)", host, port, ret, -ret, err);
                    MessageBoxA(NULL, msg, "Write Error", MB_ICONWARNING);
                    goto cleanup_ssl;
                }
            } else {
                written += ret;
            }
        }

        int last_read_ret = 0;
        while (1) {
            if (total_received + 4096 >= total_alloc) {
                total_alloc *= 2;
                char* new_buf = (char*)realloc(response_buf, total_alloc);
                if (!new_buf) break;
                response_buf = new_buf;
            }

            ret = mbedtls_ssl_read(&ssl, (unsigned char*)response_buf + total_received, 4096);
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
                break;
            }
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                    ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
                    ret == -0x7b00) {
                    continue;
                }
                last_read_ret = ret;
                break;
            }
            total_received += ret;
            response_buf[total_received] = '\0';

            if (is_http_response_complete(response_buf, total_received, NULL, NULL)) {
                break;
            }
        }

        if (total_received == 0) {
            char err[128] = {0};
            mbedtls_strerror(last_read_ret, err, sizeof(err));
            char msg[256];
            snprintf(msg, sizeof(msg), "HTTPS Read from %s:%d returned 0 bytes.\nlast_read_ret=%d (-0x%04x: %s)\nWSALastError=%d",
                     host, port, last_read_ret, -last_read_ret, err, WSAGetLastError());
            MessageBoxA(NULL, msg, "Read 0 Bytes", MB_ICONWARNING);
        }

        mbedtls_ssl_close_notify(&ssl);

    cleanup_ssl:
        mbedtls_net_free(&server_fd);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    } else {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            free(response_buf);
            return false;
        }

        DWORD tv = (timeout_ms > 0) ? (DWORD)timeout_ms : 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        struct hostent* he = gethostbyname(host);
        if (!he) {
            closesocket(s);
            free(response_buf);
            return false;
        }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons((unsigned short)port);
        memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

        if (connect(s, (struct sockaddr*)&saddr, sizeof(saddr)) != 0) {
            closesocket(s);
            free(response_buf);
            return false;
        }

        int sent = 0;
        while (sent < (int)req_len) {
            int n = send(s, request + sent, (int)req_len - sent, 0);
            if (n <= 0) break;
            sent += n;
        }

        while (1) {
            if (total_received + 4096 >= total_alloc) {
                total_alloc *= 2;
                char* new_buf = (char*)realloc(response_buf, total_alloc);
                if (!new_buf) break;
                response_buf = new_buf;
            }

            int n = recv(s, response_buf + total_received, 4096, 0);
            if (n <= 0) break;
            total_received += n;
            response_buf[total_received] = '\0';

            if (is_http_response_complete(response_buf, total_received, NULL, NULL)) {
                break;
            }
        }
        closesocket(s);
    }

    if (total_received == 0) {
        free(response_buf);
        return false;
    }

    response_buf[total_received] = '\0';
    bool ok = parse_http_response(response_buf, total_received, out_resp);
    free(response_buf);
    return ok;
}

bool http_get(const char* host, int port, const char* path, bool use_https, HttpResponse* out_resp, int timeout_ms) {
    size_t req_size = strlen(path) + strlen(host) + 512;
    char* req = (char*)malloc(req_size);
    if (!req) return false;

    int n = snprintf(req, req_size,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: Moonlight-XP/1.0\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, port);

    bool ok = http_request_raw(host, port, req, n, use_https, out_resp, timeout_ms);
    free(req);
    return ok;
}

bool http_post(const char* host, int port, const char* path, const char* post_data, size_t post_data_len, const char* content_type, bool use_https, HttpResponse* out_resp, int timeout_ms) {
    const char* ctype = content_type ? content_type : "application/x-www-form-urlencoded";
    size_t req_size = strlen(path) + strlen(host) + post_data_len + strlen(ctype) + 512;
    char* req = (char*)malloc(req_size);
    if (!req) return false;

    int n = snprintf(req, req_size,
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: Moonlight-XP/1.0\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, port, ctype, post_data_len);

    if (post_data && post_data_len > 0) {
        memcpy(req + n, post_data, post_data_len);
        n += post_data_len;
    }

    bool ok = http_request_raw(host, port, req, n, use_https, out_resp, timeout_ms);
    free(req);
    return ok;
}
