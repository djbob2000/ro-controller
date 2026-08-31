#pragma once

// Mbed TLS 4 moved the X.509 certificate writer declarations into
// mbedtls/x509_crt.h. Keep the old include name used by ro_services so the
// implementation can compile against ESP-IDF 6 without duplicating APIs.
#include "mbedtls/x509_crt.h"
