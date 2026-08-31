#pragma once

// ESP-IDF 6 / Mbed TLS 4 keeps the legacy low-level GCM surface as a private
// compatibility API. Use that implementation instead of re-declaring the
// context on top of PSA Crypto: PSA's own headers pull the same private GCM
// declarations in, so a home-grown definition causes ODR/type conflicts.

#ifndef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#endif

#include "mbedtls/private/gcm.h"
