#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int status_code;
    char* body;
    size_t body_len;
} HttpResponse;

void http_response_free(HttpResponse* resp);

// Initialize client TLS identity for HTTPS requests
bool http_client_set_tls_identity(const char* cert_pem, const char* key_pem);

// Perform an HTTP or HTTPS GET request
bool http_get(const char* host, int port, const char* path, bool use_https, HttpResponse* out_resp, int timeout_ms);

// Perform an HTTP or HTTPS POST request
bool http_post(const char* host, int port, const char* path, const char* post_data, size_t post_data_len, const char* content_type, bool use_https, HttpResponse* out_resp, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_H
