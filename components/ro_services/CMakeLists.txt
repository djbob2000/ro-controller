idf_component_register(SRCS "src/services.cpp" "src/admin_store.cpp" "src/statistics_checkpoint.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core ro_hardware nvs_flash mbedtls lwip esp_driver_i2c)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
