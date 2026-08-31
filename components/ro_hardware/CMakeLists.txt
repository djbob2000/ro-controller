idf_component_register(SRCS "src/hardware.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core esp_driver_gpio esp_driver_i2c)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
