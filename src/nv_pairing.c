#include "nv_pairing.h"
#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shlobj.h>

#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>

static void hex_encode(const unsigned char* src, size_t len, char* dst) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
}

static bool hex_decode(const char* src, unsigned char* dst, size_t max_dst_len, size_t* out_len) {
    size_t slen = strlen(src);
    if (slen % 2 != 0 || (slen / 2) > max_dst_len) return false;
    size_t count = slen / 2;
    for (size_t i = 0; i < count; i++) {
        char buf[3] = { src[i * 2], src[i * 2 + 1], '\0' };
        dst[i] = (unsigned char)strtoul(buf, NULL, 16);
    }
    if (out_len) *out_len = count;
    return true;
}

static bool extract_xml_tag(const char* xml, const char* tag, char* out_buf, size_t out_size) {
    if (!xml || !tag || !out_buf || out_size == 0) return false;
    out_buf[0] = '\0';

    size_t tag_len = strlen(tag);
    const char* p = xml;

    while (*p) {
        if (*p == '<') {
            p++;
            if (strncasecmp(p, tag, tag_len) == 0) {
                char next_char = p[tag_len];
                if (next_char == '>' || next_char == ' ' || next_char == '\t' || next_char == '\r' || next_char == '\n' || next_char == '/') {
                    const char* tag_end = strchr(p, '>');
                    if (!tag_end) return false;
                    if (*(tag_end - 1) == '/') {
                        return true;
                    }
                    const char* content_start = tag_end + 1;

                    const char* content_end = NULL;
                    for (const char* c = content_start; *c; c++) {
                        if (*c == '<' && *(c + 1) == '/' && strncasecmp(c + 2, tag, tag_len) == 0) {
                            char close_next = c[2 + tag_len];
                            if (close_next == '>' || close_next == ' ' || close_next == '\t' || close_next == '\r' || close_next == '\n') {
                                content_end = c;
                                break;
                            }
                        }
                    }

                    if (content_end) {
                        size_t len = (size_t)(content_end - content_start);
                        if (len >= out_size) len = out_size - 1;
                        memcpy(out_buf, content_start, len);
                        out_buf[len] = '\0';
                        return true;
                    }
                }
            }
        }
        p++;
    }
    return false;
}

static void aes_ecb_encrypt(const unsigned char* key, const unsigned char* in, unsigned char* out, size_t len) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    for (size_t i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in + i, out + i);
    }
    mbedtls_aes_free(&ctx);
}

static void aes_ecb_decrypt(const unsigned char* key, const unsigned char* in, unsigned char* out, size_t len) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    for (size_t i = 0; i < len; i += 16) {
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in + i, out + i);
    }
    mbedtls_aes_free(&ctx);
}

static void parse_host_port(const char* input, char* out_host, size_t host_size, int* out_port) {
    strncpy(out_host, input, host_size);
    char* colon = strrchr(out_host, ':');
    if (colon) {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && out_port) *out_port = p;
    }
}

bool nv_init_client_identity(NvClientIdentity* ident, const char* app_data_dir) {
    if (!ident) return false;
    memset(ident, 0, sizeof(NvClientIdentity));

    psa_crypto_init();

    snprintf(ident->unique_id, sizeof(ident->unique_id), "0123456789ABCDEF");

    char app_data[MAX_PATH];
    if (app_data_dir) {
        strncpy(app_data, app_data_dir, sizeof(app_data));
    } else {
        GetModuleFileNameA(NULL, app_data, sizeof(app_data));
        char* last_slash = strrchr(app_data, '\\');
        if (last_slash) *last_slash = '\0';
    }

    snprintf(ident->cert_path, sizeof(ident->cert_path), "%s\\client.crt", app_data);
    snprintf(ident->key_path, sizeof(ident->key_path), "%s\\client.key", app_data);

    // Try loading existing files
    FILE* fc = fopen(ident->cert_path, "rb");
    FILE* fk = fopen(ident->key_path, "rb");
    if (fc && fk) {
        fseek(fc, 0, SEEK_END);
        size_t csz = ftell(fc);
        fseek(fc, 0, SEEK_SET);

        fseek(fk, 0, SEEK_END);
        size_t ksz = ftell(fk);
        fseek(fk, 0, SEEK_SET);

        ident->cert_pem = (char*)malloc(csz + 1);
        fread(ident->cert_pem, 1, csz, fc);
        ident->cert_pem[csz] = '\0';
        fclose(fc);

        ident->key_pem = (char*)malloc(ksz + 1);
        fread(ident->key_pem, 1, ksz, fk);
        ident->key_pem[ksz] = '\0';
        fclose(fk);

        http_client_set_tls_identity(ident->cert_pem, ident->key_pem);
        return true;
    }
    if (fc) fclose(fc);
    if (fk) fclose(fk);

    // Generate new RSA 2048 key and self-signed certificate
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;

    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    const char* pers = "moonlight_xp_client";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers)) != 0) {
        goto error;
    }

    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) {
        goto error;
    }

    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &ctr_drbg, 2048, 65537) != 0) {
        goto error;
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    mbedtls_mpi_read_string(&serial, 10, "1");
    mbedtls_x509write_crt_set_serial(&crt, &serial);

    mbedtls_x509write_crt_set_subject_name(&crt, "CN=NVIDIA GameStream Client,O=NVIDIA Corporation,C=US");
    mbedtls_x509write_crt_set_issuer_name(&crt, "CN=NVIDIA GameStream Client,O=NVIDIA Corporation,C=US");

    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20401231235959");

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    unsigned char key_pem_buf[4096];
    if (mbedtls_pk_write_key_pem(&key, key_pem_buf, sizeof(key_pem_buf)) != 0) {
        goto error;
    }
    ident->key_pem = strdup((char*)key_pem_buf);

    unsigned char cert_pem_buf[4096];
    if (mbedtls_x509write_crt_pem(&crt, cert_pem_buf, sizeof(cert_pem_buf), mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        goto error;
    }
    ident->cert_pem = strdup((char*)cert_pem_buf);

    // Save to disk if possible
    fk = fopen(ident->key_path, "wb");
    if (fk) {
        fwrite(ident->key_pem, 1, strlen(ident->key_pem), fk);
        fclose(fk);
    }

    fc = fopen(ident->cert_path, "wb");
    if (fc) {
        fwrite(ident->cert_pem, 1, strlen(ident->cert_pem), fc);
        fclose(fc);
    }

    http_client_set_tls_identity(ident->cert_pem, ident->key_pem);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return true;

error:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return false;
}

bool nv_get_server_info(const char* input_host, int https_port, NvServerInfo* out_info) {
    if (!input_host || !out_info) return false;
    memset(out_info, 0, sizeof(NvServerInfo));

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    strncpy(out_info->address, host, sizeof(out_info->address));
    out_info->http_port = 47989;
    out_info->https_port = (specified_port > 0) ? specified_port : (https_port > 0 ? https_port : 47984);

    HttpResponse resp;
    char path[256];
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=0123456789ABCDEF");

    // Step 1: Query HTTP port 47989 for general server metadata & actual HTTPS port
    memset(&resp, 0, sizeof(resp));
    bool ok = http_get(host, out_info->http_port, path, false, &resp, 3000);
    if (ok && resp.body) {
        extract_xml_tag(resp.body, "hostname", out_info->hostname, sizeof(out_info->hostname));
        extract_xml_tag(resp.body, "appversion", out_info->server_version, sizeof(out_info->server_version));

        char https_port_str[16] = {0};
        if (extract_xml_tag(resp.body, "HttpsPort", https_port_str, sizeof(https_port_str))) {
            int hp = atoi(https_port_str);
            if (hp > 0 && specified_port == 0) out_info->https_port = hp;
        }
        http_response_free(&resp);
    }

    // Step 2: Query HTTPS port with client certificate for authenticated PairStatus
    HttpResponse s_resp;
    memset(&s_resp, 0, sizeof(s_resp));
    ok = http_get(host, out_info->https_port, path, true, &s_resp, 5000);

    if (ok && s_resp.body && s_resp.status_code == 200) {
        char pair_status[16] = {0};
        char paired_tag[16] = {0};
        extract_xml_tag(s_resp.body, "PairStatus", pair_status, sizeof(pair_status));
        extract_xml_tag(s_resp.body, "paired", paired_tag, sizeof(paired_tag));
        out_info->is_paired = (atoi(pair_status) == 1 || atoi(paired_tag) == 1);

        if (strlen(out_info->hostname) == 0) {
            extract_xml_tag(s_resp.body, "hostname", out_info->hostname, sizeof(out_info->hostname));
            extract_xml_tag(s_resp.body, "appversion", out_info->server_version, sizeof(out_info->server_version));
        }
        http_response_free(&s_resp);
    } else {
        if (ok) http_response_free(&s_resp);
        out_info->is_paired = false;
    }

    return (strlen(out_info->hostname) > 0 || out_info->is_paired);
}

bool nv_pair_server(const char* input_host, int https_port, const NvClientIdentity* ident, const char* pin) {
    if (!input_host || !ident || !pin || !ident->cert_pem || !ident->key_pem) {
        return false;
    }

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt client_crt;
    mbedtls_pk_context client_key;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&client_crt);
    mbedtls_pk_init(&client_key);

    const char* pers = "moonlight_pair";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers)) != 0) {
        return false;
    }

    if (mbedtls_x509_crt_parse(&client_crt, (const unsigned char*)ident->cert_pem, strlen(ident->cert_pem) + 1) != 0) {
        return false;
    }

    if (mbedtls_pk_parse_key(&client_key, (const unsigned char*)ident->key_pem, strlen(ident->key_pem) + 1, NULL, 0,
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        mbedtls_x509_crt_free(&client_crt);
        return false;
    }

    unsigned char salt[16];
    mbedtls_ctr_drbg_random(&ctr_drbg, salt, 16);
    char salt_hex[33];
    hex_encode(salt, 16, salt_hex);

    char* client_cert_hex = (char*)malloc(strlen(ident->cert_pem) * 2 + 1);
    hex_encode((const unsigned char*)ident->cert_pem, strlen(ident->cert_pem), client_cert_hex);

    size_t path_len = strlen(ident->unique_id) + strlen(salt_hex) + strlen(client_cert_hex) + 256;
    char* path = (char*)malloc(path_len);
    snprintf(path, path_len,
             "/pair?uniqueid=%s&devicename=ROOT&updateState=1&phrase=getservercert&salt=%s&clientcert=%s",
             ident->unique_id, salt_hex, client_cert_hex);
    free(client_cert_hex);

    HttpResponse resp;
    int pair_ports[] = { (specified_port > 0 ? specified_port : 47989), 37989, 47989, (https_port > 0 ? https_port : 47984), 37984 };
    bool use_https = false;
    int active_port = 0;
    bool ok = false;

    // Try finding the active pairing endpoint
    for (int i = 0; i < 5; i++) {
        int p = pair_ports[i];
        if (p <= 0) continue;
        bool https_try = (p == 47984 || p == 37984 || (specified_port > 0 && specified_port == https_port));
        memset(&resp, 0, sizeof(resp));
        ok = http_get(host, p, path, https_try, &resp, 120000);
        if (ok && resp.status_code == 200 && resp.body) {
            active_port = p;
            use_https = https_try;
            break;
        }
        if (ok) http_response_free(&resp);
    }
    free(path);

    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    char plaincert_hex[8192] = {0};
    extract_xml_tag(resp.body, "plaincert", plaincert_hex, sizeof(plaincert_hex));
    http_response_free(&resp);

    if (strlen(plaincert_hex) == 0) {
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    // Derive AES Key = SHA256(salt + PIN)[0..15]
    unsigned char salt_pin[64];
    memcpy(salt_pin, salt, 16);
    memcpy(salt_pin + 16, pin, strlen(pin));

    unsigned char sha_key[32];
    mbedtls_sha256(salt_pin, 16 + strlen(pin), sha_key, 0);

    unsigned char aes_key[16];
    memcpy(aes_key, sha_key, 16);

    // Phase 2: clientchallenge
    unsigned char client_challenge[16];
    mbedtls_ctr_drbg_random(&ctr_drbg, client_challenge, 16);

    unsigned char enc_client_challenge[16];
    aes_ecb_encrypt(aes_key, client_challenge, enc_client_challenge, 16);

    char enc_client_challenge_hex[33];
    hex_encode(enc_client_challenge, 16, enc_client_challenge_hex);

    char path_buf[1024];
    snprintf(path_buf, sizeof(path_buf), "/pair?uniqueid=%s&devicename=ROOT&updateState=1&clientchallenge=%s",
             ident->unique_id, enc_client_challenge_hex);

    memset(&resp, 0, sizeof(resp));
    ok = http_get(host, active_port, path_buf, use_https, &resp, 10000);
    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    char challenge_response_hex[256] = {0};
    extract_xml_tag(resp.body, "challengeresponse", challenge_response_hex, sizeof(challenge_response_hex));
    http_response_free(&resp);

    if (strlen(challenge_response_hex) == 0) {
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    // Decrypt challenge response
    unsigned char enc_challenge_resp[128];
    size_t enc_resp_len = 0;
    hex_decode(challenge_response_hex, enc_challenge_resp, sizeof(enc_challenge_resp), &enc_resp_len);

    unsigned char dec_challenge_resp[128];
    aes_ecb_decrypt(aes_key, enc_challenge_resp, dec_challenge_resp, enc_resp_len);

    unsigned char server_challenge[16];
    memcpy(server_challenge, dec_challenge_resp + 32, 16);

    // Phase 3: serverchallengeresp
    unsigned char client_secret[16];
    mbedtls_ctr_drbg_random(&ctr_drbg, client_secret, 16);

    size_t client_sig_len = client_crt.MBEDTLS_PRIVATE(sig).len;
    unsigned char* client_sig_ptr = client_crt.MBEDTLS_PRIVATE(sig).p;

    size_t hash_input_len = 16 + client_sig_len + 16;
    unsigned char* hash_input = (unsigned char*)malloc(hash_input_len);
    memcpy(hash_input, server_challenge, 16);
    memcpy(hash_input + 16, client_sig_ptr, client_sig_len);
    memcpy(hash_input + 16 + client_sig_len, client_secret, 16);

    unsigned char client_hash[32];
    mbedtls_sha256(hash_input, hash_input_len, client_hash, 0);
    free(hash_input);

    unsigned char enc_client_hash[32];
    aes_ecb_encrypt(aes_key, client_hash, enc_client_hash, 32);

    char enc_client_hash_hex[65];
    hex_encode(enc_client_hash, 32, enc_client_hash_hex);

    snprintf(path_buf, sizeof(path_buf), "/pair?uniqueid=%s&devicename=ROOT&updateState=1&serverchallengeresp=%s",
             ident->unique_id, enc_client_hash_hex);

    memset(&resp, 0, sizeof(resp));
    ok = http_get(host, active_port, path_buf, use_https, &resp, 10000);
    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }
    http_response_free(&resp);

    // Phase 4: clientpairingsecret
    unsigned char secret_sha[32];
    mbedtls_sha256(client_secret, 16, secret_sha, 0);

    unsigned char client_secret_sig[512];
    size_t secret_sig_len = 0;
    if (mbedtls_pk_sign(&client_key, MBEDTLS_MD_SHA256, secret_sha, 32,
                        client_secret_sig, sizeof(client_secret_sig), &secret_sig_len,
                        mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        mbedtls_x509_crt_free(&client_crt);
        mbedtls_pk_free(&client_key);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    size_t pairing_secret_len = 16 + secret_sig_len;
    unsigned char* pairing_secret = (unsigned char*)malloc(pairing_secret_len);
    memcpy(pairing_secret, client_secret, 16);
    memcpy(pairing_secret + 16, client_secret_sig, secret_sig_len);

    char* pairing_secret_hex = (char*)malloc(pairing_secret_len * 2 + 1);
    hex_encode(pairing_secret, pairing_secret_len, pairing_secret_hex);
    free(pairing_secret);

    size_t p4_path_len = strlen(ident->unique_id) + strlen(pairing_secret_hex) + 128;
    char* p4_path = (char*)malloc(p4_path_len);
    snprintf(p4_path, p4_path_len, "/pair?uniqueid=%s&devicename=ROOT&updateState=1&clientpairingsecret=%s",
             ident->unique_id, pairing_secret_hex);
    free(pairing_secret_hex);

    memset(&resp, 0, sizeof(resp));
    ok = http_get(host, active_port, p4_path, use_https, &resp, 10000);
    free(p4_path);

    char paired_str[16] = {0};
    if (ok && resp.body) {
        extract_xml_tag(resp.body, "paired", paired_str, sizeof(paired_str));
    }
    if (ok) http_response_free(&resp);

    mbedtls_x509_crt_free(&client_crt);
    mbedtls_pk_free(&client_key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return (atoi(paired_str) == 1);
}

bool nv_get_app_list(const char* input_host, int https_port, const NvClientIdentity* ident, NvServerInfo* server_info) {
    if (!input_host || !ident || !server_info) return false;

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    HttpResponse resp;
    char path[256];
    snprintf(path, sizeof(path), "/applist?uniqueid=%s", ident->unique_id);

    int port = (specified_port > 0) ? specified_port : (https_port > 0 ? https_port : 47984);
    memset(&resp, 0, sizeof(resp));
    bool ok = http_get(host, port, path, true, &resp, 5000);
    if (!ok || !resp.body) {
        // Try fallback port 37984 if 47984 failed
        if (port == 47984) {
            port = 37984;
            ok = http_get(host, port, path, true, &resp, 5000);
        }
    }

    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        return false;
    }

    server_info->app_count = 0;
    const char* cur = resp.body;
    while (cur && *cur && server_info->app_count < 32) {
        const char* app_start = NULL;
        for (const char* c = cur; *c; c++) {
            if (*c == '<' && strncasecmp(c + 1, "App", 3) == 0 && (c[4] == '>' || c[4] == ' ' || c[4] == '\t' || c[4] == '\r' || c[4] == '\n')) {
                app_start = c;
                break;
            }
        }
        if (!app_start) break;

        const char* app_end = NULL;
        for (const char* c = app_start; *c; c++) {
            if (*c == '<' && *(c + 1) == '/' && strncasecmp(c + 2, "App", 3) == 0 && (c[5] == '>' || c[5] == ' ' || c[5] == '\t' || c[5] == '\r' || c[5] == '\n')) {
                const char* close_bracket = strchr(c, '>');
                if (close_bracket) {
                    app_end = close_bracket + 1;
                    break;
                }
            }
        }
        if (!app_end) break;

        size_t app_block_len = app_end - app_start;
        char* app_block = (char*)malloc(app_block_len + 1);
        memcpy(app_block, app_start, app_block_len);
        app_block[app_block_len] = '\0';

        char title[128] = {0};
        char id_str[32] = {0};

        if ((extract_xml_tag(app_block, "AppTitle", title, sizeof(title)) ||
             extract_xml_tag(app_block, "title", title, sizeof(title)) ||
             extract_xml_tag(app_block, "Name", title, sizeof(title))) &&
            (extract_xml_tag(app_block, "ID", id_str, sizeof(id_str)) ||
             extract_xml_tag(app_block, "id", id_str, sizeof(id_str)))) {
            int idx = server_info->app_count;
            strncpy(server_info->apps[idx].name, title, sizeof(server_info->apps[idx].name));
            server_info->apps[idx].id = atoi(id_str);
            server_info->app_count++;
        }

        free(app_block);
        cur = app_end;
    }

    http_response_free(&resp);
    return (server_info->app_count > 0);
}

bool nv_launch_app(const char* input_host, int https_port, const NvClientIdentity* ident,
                  int app_id, int width, int height, int fps, int bitrate_kbps,
                  char* out_rtsp_session_url, size_t out_url_size) {
    if (!input_host || !ident || !out_rtsp_session_url) return false;

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    HttpResponse resp;
    char path[1024];
    snprintf(path, sizeof(path),
             "/launch?uniqueid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=0&rikey=30313233343536373839414243444546&rikeyid=0&localAudioPlayMode=0",
             ident->unique_id, app_id, width, height, fps);

    int port = (specified_port > 0) ? specified_port : (https_port > 0 ? https_port : 47984);
    memset(&resp, 0, sizeof(resp));
    bool ok = http_get(host, port, path, true, &resp, 10000);
    if (!ok || !resp.body) {
        if (port == 47984) {
            port = 37984;
            ok = http_get(host, port, path, true, &resp, 10000);
        }
    }

    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        return false;
    }

    char session_url[256] = {0};
    if (extract_xml_tag(resp.body, "sessionUrl0", session_url, sizeof(session_url))) {
        strncpy(out_rtsp_session_url, session_url, out_url_size);
    } else {
        out_rtsp_session_url[0] = '\0';
    }

    http_response_free(&resp);
    return true;
}

bool nv_resume_app(const char* input_host, int https_port, const NvClientIdentity* ident,
                  char* out_rtsp_session_url, size_t out_url_size) {
    if (!input_host || !ident || !out_rtsp_session_url) return false;

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    HttpResponse resp;
    char path[512];
    snprintf(path, sizeof(path),
             "/resume?uniqueid=%s&rikey=30313233343536373839414243444546&rikeyid=0",
             ident->unique_id);

    int port = (specified_port > 0) ? specified_port : (https_port > 0 ? https_port : 47984);
    memset(&resp, 0, sizeof(resp));
    bool ok = http_get(host, port, path, true, &resp, 10000);
    if (!ok || !resp.body) {
        if (port == 47984) {
            port = 37984;
            ok = http_get(host, port, path, true, &resp, 10000);
        }
    }

    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        return false;
    }

    char session_url[256] = {0};
    if (extract_xml_tag(resp.body, "sessionUrl0", session_url, sizeof(session_url))) {
        strncpy(out_rtsp_session_url, session_url, out_url_size);
    }

    http_response_free(&resp);
    return true;
}

bool nv_quit_app(const char* input_host, int https_port, const NvClientIdentity* ident) {
    if (!input_host || !ident) return false;

    char host[128];
    int specified_port = 0;
    parse_host_port(input_host, host, sizeof(host), &specified_port);

    HttpResponse resp;
    char path[256];
    snprintf(path, sizeof(path), "/cancel?uniqueid=%s", ident->unique_id);

    int port = (specified_port > 0) ? specified_port : (https_port > 0 ? https_port : 47984);
    memset(&resp, 0, sizeof(resp));
    bool ok = http_get(host, port, path, true, &resp, 5000);
    if (!ok || !resp.body) {
        if (port == 47984) {
            port = 37984;
            ok = http_get(host, port, path, true, &resp, 5000);
        }
    }

    if (!ok || !resp.body) {
        if (ok) http_response_free(&resp);
        return false;
    }

    http_response_free(&resp);
    return true;
}
