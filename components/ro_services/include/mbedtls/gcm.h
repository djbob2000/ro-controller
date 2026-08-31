#pragma once

// ESP-IDF 6 ships Mbed TLS 4 with the legacy public mbedtls/gcm.h API removed.
// ro_services still uses the compact Mbed TLS 3 GCM call shape for encrypted
// backup envelopes, so keep that call site stable while routing the actual
// cryptography through the supported PSA Crypto API.

#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509write.h"
#include "psa/crypto.h"

#include <cstdlib>
#include <cstring>

// These identifiers were supplied by the removed legacy GCM/cipher headers.
// They are intentionally local compatibility constants; the PSA adapter below
// only accepts AES and maps it to PSA_KEY_TYPE_AES / PSA_ALG_GCM.
#ifndef MBEDTLS_CIPHER_ID_AES
#define MBEDTLS_CIPHER_ID_AES 1
#endif
#ifndef MBEDTLS_GCM_ENCRYPT
#define MBEDTLS_GCM_ENCRYPT 1
#endif
#ifndef MBEDTLS_GCM_DECRYPT
#define MBEDTLS_GCM_DECRYPT 0
#endif

typedef struct mbedtls_gcm_context {
    psa_key_id_t key_id;
    bool key_loaded;
} mbedtls_gcm_context;

static inline void mbedtls_gcm_init(mbedtls_gcm_context* ctx) {
    if (!ctx) return;
    ctx->key_id = 0;
    ctx->key_loaded = false;
}

static inline void mbedtls_gcm_free(mbedtls_gcm_context* ctx) {
    if (!ctx) return;
    if (ctx->key_loaded) {
        (void)psa_destroy_key(ctx->key_id);
    }
    ctx->key_id = 0;
    ctx->key_loaded = false;
}

static inline int mbedtls_gcm_setkey(mbedtls_gcm_context* ctx, int cipher,
                                     const unsigned char* key,
                                     unsigned int keybits) {
    if (!ctx || !key || cipher != MBEDTLS_CIPHER_ID_AES ||
        (keybits != 128U && keybits != 192U && keybits != 256U)) {
        return -1;
    }
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    if (ctx->key_loaded) {
        (void)psa_destroy_key(ctx->key_id);
        ctx->key_loaded = false;
        ctx->key_id = 0;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, keybits);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    const psa_status_t status = psa_import_key(
        &attributes, key, keybits / 8U, &ctx->key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ctx->key_id = 0;
        return -1;
    }
    ctx->key_loaded = true;
    return 0;
}

static inline int mbedtls_gcm_crypt_and_tag(
    mbedtls_gcm_context* ctx, int mode, size_t length,
    const unsigned char* iv, size_t iv_len,
    const unsigned char* add, size_t add_len,
    const unsigned char* input, unsigned char* output,
    size_t tag_len, unsigned char* tag) {
    if (!ctx || !ctx->key_loaded || mode != MBEDTLS_GCM_ENCRYPT ||
        !iv || !input || !output || !tag || tag_len == 0U) {
        return -1;
    }

    const psa_algorithm_t alg = tag_len == 16U
        ? PSA_ALG_GCM
        : PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, tag_len);
    const size_t result_capacity = length + tag_len;
    auto* result = static_cast<unsigned char*>(std::malloc(result_capacity));
    if (!result) return -1;

    size_t result_len = 0;
    const psa_status_t status = psa_aead_encrypt(
        ctx->key_id, alg, iv, iv_len,
        add, add_len, input, length,
        result, result_capacity, &result_len);
    if (status == PSA_SUCCESS && result_len == result_capacity) {
        std::memcpy(output, result, length);
        std::memcpy(tag, result + length, tag_len);
    }
    std::free(result);
    return status == PSA_SUCCESS && result_len == result_capacity ? 0 : -1;
}

static inline int mbedtls_gcm_auth_decrypt(
    mbedtls_gcm_context* ctx, size_t length,
    const unsigned char* iv, size_t iv_len,
    const unsigned char* add, size_t add_len,
    const unsigned char* tag, size_t tag_len,
    const unsigned char* input, unsigned char* output) {
    if (!ctx || !ctx->key_loaded || !iv || !tag || !input || !output || tag_len == 0U) {
        return -1;
    }

    const psa_algorithm_t alg = tag_len == 16U
        ? PSA_ALG_GCM
        : PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, tag_len);
    const size_t sealed_len = length + tag_len;
    auto* sealed = static_cast<unsigned char*>(std::malloc(sealed_len));
    if (!sealed) return -1;
    std::memcpy(sealed, input, length);
    std::memcpy(sealed + length, tag, tag_len);

    size_t output_len = 0;
    const psa_status_t status = psa_aead_decrypt(
        ctx->key_id, alg, iv, iv_len,
        add, add_len, sealed, sealed_len,
        output, length, &output_len);
    std::free(sealed);
    return status == PSA_SUCCESS && output_len == length ? 0 : -1;
}