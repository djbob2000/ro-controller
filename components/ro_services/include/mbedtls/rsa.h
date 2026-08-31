#pragma once

// ESP-IDF 6 / Mbed TLS 4 removed the old public RSA setup helpers, but its PK
// layer can copy a PSA key into an mbedtls_pk_context. Keep the narrow legacy
// call shape used by ro_services without redefining mbedtls_mpi: the latter is
// still provided by Mbed TLS' compatibility bignum header and redefining it
// conflicts with the X.509 implementation.

#ifndef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#endif

#include "mbedtls/private/bignum.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"

#include <stddef.h>

#ifndef MBEDTLS_PK_RSA
#define MBEDTLS_PK_RSA 1
#endif

static inline const void* ro_mbedtls_pk_info_from_type_compat(int type) {
    (void) type;
    return nullptr;
}

static inline int ro_mbedtls_pk_setup_compat(mbedtls_pk_context* key, const void* info) {
    (void) key;
    (void) info;
    // mbedtls_pk_init() already produced an empty destination; the following
    // compatibility rsa_gen_key call fills it from a temporary PSA key.
    return 0;
}

static inline mbedtls_pk_context* ro_mbedtls_pk_rsa_compat(mbedtls_pk_context* key) {
    return key;
}

static inline int ro_mbedtls_rsa_gen_key_compat(
    mbedtls_pk_context* key,
    int (*f_rng)(void*, unsigned char*, size_t),
    void* p_rng,
    unsigned int nbits,
    int exponent) {
    (void) f_rng;
    (void) p_rng;

    if (key == nullptr || nbits < 2048U || exponent != 65537) return -1;

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return static_cast<int>(status);

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, static_cast<psa_key_bits_t>(nbits));
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    status = psa_generate_key(&attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) return static_cast<int>(status);

    const int rc = mbedtls_pk_copy_from_psa(key_id, key);
    const psa_status_t destroy_status = psa_destroy_key(key_id);
    if (rc != 0) return rc;
    return destroy_status == PSA_SUCCESS ? 0 : static_cast<int>(destroy_status);
}

#define mbedtls_pk_info_from_type(type) ro_mbedtls_pk_info_from_type_compat(static_cast<int>(type))
#define mbedtls_pk_setup(key, info) ro_mbedtls_pk_setup_compat((key), (info))
#define mbedtls_pk_rsa(key) ro_mbedtls_pk_rsa_compat(&(key))
#define mbedtls_rsa_gen_key(key, f_rng, p_rng, nbits, exponent) \
    ro_mbedtls_rsa_gen_key_compat((key), (f_rng), (p_rng), (nbits), (exponent))
