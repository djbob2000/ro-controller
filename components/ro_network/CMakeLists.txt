idf_component_register(SRCS "src/network.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core ro_services esp_wifi esp_event esp_netif esp_http_server esp_https_server app_update mbedtls esp_timer espressif__mqtt)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# ESP-IDF 6 extended httpd_uri_t and made content_len unsigned. The existing
# initialization/check remains safe; keep these two migration warnings from
# becoming hard errors until the compatibility code is simplified.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers
    -Wno-error=type-limits)
