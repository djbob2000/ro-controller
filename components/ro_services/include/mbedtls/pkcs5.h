#pragma once

// Mbed TLS 4 no longer exposes the legacy public mbedtls/pkcs5.h API used by
// ro_services. Preserve only its SHA-256 PBKDF2 call surface and implement the
// derivation with PSA Crypto, matching ESP-IDF 6's own PBKDF2 implementation.

#include "mbedtls/md.h"
#include "psa/crypto.h"

#include <cstddef>
#include <cstdint>

static inline int ro_psa_pbkdf2_sha256(
    const unsigned char* password, size_t plen,
    const unsigned char* salt, size_t slen,
    unsigned int iteration_count,
    size_t key_length,
    unsigned char* output) {
    if (!password || !salt || !output || iteration_count == 0U || key_length == 0U) {
        return -1;
    }
    if (psa_crypto_init() != PSA_SUCCESS) return -1;

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_integer(
        &op, PSA_KEY_DERIVATION_INPUT_COST, iteration_count);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_bytes(
        &op, PSA_KEY_DERIVATION_INPUT_SALT, salt, slen);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_bytes(
        &op, PSA_KEY_DERIVATION_INPUT_PASSWORD, password, plen);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_output_bytes(&op, output, key_length);

cleanup:
    (void)psa_key_derivation_abort(&op);
    return status == PSA_SUCCESS ? 0 : -1;
}

static inline int mbedtls_pkcs5_pbkdf2_hmac(
    mbedtls_md_context_t* ctx,
    const unsigned char* password, size_t plen,
    const unsigned char* salt, size_t slen,
    unsigned int iteration_count,
    size_t key_length,
    unsigned char* output) {
    // ro_services always initializes this context for SHA-256 immediately
    // before calling PBKDF2. Mbed TLS 4 hides the legacy context internals, so
    // the compatibility layer deliberately supports only that single use case.
    if (!ctx) return -1;
    return ro_psa_pbkdf2_sha256(
        password, plen, salt, slen, iteration_count, key_length, output);
}

static inline int mbedtls_pkcs5_pbkdf2_hmac_ext(
    mbedtls_md_type_t md_type,
    const unsigned char* password, size_t plen,
    const unsigned char* salt, size_t slen,
    unsigned int iteration_count,
    uint32_t key_length,
    unsigned char* output) {
    if (md_type != MBEDTLS_MD_SHA256) return -1;
    return ro_psa_pbkdf2_sha256(
        password, plen, salt, slen, iteration_count, key_length, output);
}