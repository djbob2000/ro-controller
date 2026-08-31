#pragma once

// Mbed TLS 4 moved the X.509 certificate writer declarations into
// mbedtls/x509_crt.h and removed the legacy MPI serial/RNG callback forms.
// Preserve the narrow legacy surface used by ro_services while forwarding to
// the native Mbed TLS 4 APIs.

#include "mbedtls/x509_crt.h"
#include "mbedtls/rsa.h"

static inline int ro_mbedtls_x509write_crt_set_serial_compat(
    mbedtls_x509write_cert* ctx, const mbedtls_mpi* serial) {
    if (ctx == NULL || serial == NULL || serial->len == 0) return -1;
    return mbedtls_x509write_crt_set_serial_raw(ctx, serial->bytes, serial->len);
}

static inline int ro_mbedtls_x509write_crt_pem_compat(
    mbedtls_x509write_cert* ctx,
    unsigned char* buf,
    size_t size,
    int (*f_rng)(void*, unsigned char*, size_t),
    void* p_rng) {
    (void) f_rng;
    (void) p_rng;
    return mbedtls_x509write_crt_pem(ctx, buf, size);
}

#define mbedtls_x509write_crt_set_serial(ctx, serial) \
    ro_mbedtls_x509write_crt_set_serial_compat((ctx), (serial))
#define mbedtls_x509write_crt_pem(ctx, buf, size, f_rng, p_rng) \
    ro_mbedtls_x509write_crt_pem_compat((ctx), (buf), (size), (f_rng), (p_rng))
