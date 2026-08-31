#pragma once

// Compatibility bridge for the small legacy RSA surface still used by
// ro_services. ESP-IDF 6 ships Mbed TLS 4, where low-level RSA and MPI APIs
// were removed in favour of PSA Crypto. Keep the call site stable while
// generating the key through PSA and importing it into mbedtls_pk_context.

#include "mbedtls/pk.h"
#include "psa/crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MBEDTLS_PK_RSA
#define MBEDTLS_PK_RSA 1
#endif

typedef struct ro_mbedtls_compat_mpi {
    unsigned char bytes[20];
    size_t len;
} mbedtls_mpi;

static inline void mbedtls_mpi_init(mbedtls_mpi* value) {
    if (value != NULL) {
        memset(value->bytes, 0, sizeof(value->bytes));
        value->len = 0;
    }
}

static inline void mbedtls_mpi_free(mbedtls_mpi* value) {
    if (value != NULL) {
        memset(value->bytes, 0, sizeof(value->bytes));
        value->len = 0;
    }
}

static inline int mbedtls_mpi_lset(mbedtls_mpi* value, int64_t number) {
    if (value == NULL || number < 0) return -1;

    memset(value->bytes, 0, sizeof(value->bytes));
    if (number == 0) {
        value->len = 1;
        return 0;
    }

    unsigned char tmp[sizeof(uint64_t)] = {};
    size_t used = 0;
    uint64_t current = (uint64_t) number;
    while (current != 0 && used < sizeof(tmp)) {
        tmp[sizeof(tmp) - 1 - used] = (unsigned char) (current & 0xffU);
        current >>= 8U;
        ++used;
    }
    memcpy(value->bytes, tmp + sizeof(tmp) - used, used);
    value->len = used;
    return 0;
}

static inline const void* ro_mbedtls_pk_info_from_type_compat(int type) {
    (void) type;
    return NULL;
}

static inline int ro_mbedtls_pk_setup_compat(mbedtls_pk_context* key, const void* info) {
    (void) key;
    (void) info;
    // mbedtls_pk_init() already prepared the destination. The following
    // rsa_gen_key compatibility call populates it from a PSA key.
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

    if (key == NULL || nbits < 2048U || exponent != 65537) return -1;

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return (int) status;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, (psa_key_bits_t) nbits);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    status = psa_generate_key(&attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) return (int) status;

    const int rc = mbedtls_pk_copy_from_psa(key_id, key);
    const psa_status_t destroy_status = psa_destroy_key(key_id);
    if (rc != 0) return rc;
    return destroy_status == PSA_SUCCESS ? 0 : (int) destroy_status;
}

#define mbedtls_pk_info_from_type(type) ro_mbedtls_pk_info_from_type_compat((int) (type))
#define mbedtls_pk_setup(key, info) ro_mbedtls_pk_setup_compat((key), (info))
#define mbedtls_pk_rsa(key) ro_mbedtls_pk_rsa_compat(&(key))
#define mbedtls_rsa_gen_key(key, f_rng, p_rng, nbits, exponent) \
    ro_mbedtls_rsa_gen_key_compat((key), (f_rng), (p_rng), (nbits), (exponent))
