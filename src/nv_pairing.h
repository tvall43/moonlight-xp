#ifndef NV_PAIRING_H
#define NV_PAIRING_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_APPS 128
#define MAX_APP_NAME 128

typedef struct {
    int id;
    char name[MAX_APP_NAME];
    bool is_hdr;
} NvApp;

typedef struct {
    char hostname[128];
    char address[128];
    int https_port;
    int http_port;
    char server_version[32];
    bool is_paired;
    int app_count;
    NvApp apps[MAX_APPS];
} NvServerInfo;

typedef struct {
    char unique_id[64];
    char cert_path[260];
    char key_path[260];
    char* cert_pem;
    char* key_pem;
} NvClientIdentity;

// Initialize client identity (loads or generates client certificate & key)
bool nv_init_client_identity(NvClientIdentity* ident, const char* cert_dir);

// Query server info (checks pairing status and versions)
bool nv_get_server_info(const char* host, int https_port, NvServerInfo* out_info);

// Perform PIN pairing handshake with server
// The user enters the pin into Sunshine web UI / GFE popup
bool nv_pair_server(const char* host, int https_port, const NvClientIdentity* ident, const char* pin);

// Fetch application list from paired server
bool nv_get_app_list(const char* host, int https_port, const NvClientIdentity* ident, NvServerInfo* server_info);

// Launch an app (e.g. app_id or Desktop) and retrieve RTSP session URL / params
bool nv_launch_app(const char* host, int https_port, const NvClientIdentity* ident, int app_id, int width, int height, int fps, int bitrate_kbps, char* out_session_url, size_t session_url_len);

// Quit / cancel current running app on server
bool nv_quit_app(const char* host, int https_port, const NvClientIdentity* ident);

#endif // NV_PAIRING_H
