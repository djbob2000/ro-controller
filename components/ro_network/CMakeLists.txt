idf_component_register(SRCS "src/network.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core ro_services esp_wifi esp_event esp_netif esp_http_server esp_https_server mqtt app_update mbedtls json mdns esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
