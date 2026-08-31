#pragma once

// Mbed TLS 4 moved the certificate writer declarations into x509_crt.h and
// changed serial/PEM helpers. Preserve the small legacy surface used by
// ro_services while forwarding to the native ESP-IDF 6 implementation.

#include "mbedtls/x509_crt.h"
#include "mbedtls/rsa.h"

#include <array>

static inline int ro_mbedtls_x509write_crt_set_serial_compat(
    mbedtls_x509write_cert* ctx, const mbedtls_mpi* serial) {
    if (ctx == nullptr || serial == nullptr) return MBEDTLS_ERR_X509_BAD_INPUT_DATA;

    const size_t serial_len = mbedtls_mpi_size(serial);
    if (serial_len == 0 || serial_len > MBEDTLS_X509_RFC5280_MAX_SERIAL_LEN) {
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    }

    std::array<unsigned char, MBEDTLS_X509_RFC5280_MAX_SERIAL_LEN> raw{};
    const int rc = mbedtls_mpi_write_binary(serial, raw.data(), serial_len);
    if (rc != 0) return rc;
    return mbedtls_x509write_crt_set_serial_raw(ctx, raw.data(), serial_len);
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
