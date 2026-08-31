idf_component_register(SRCS "src/hardware.cpp" "src/flow_meter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core esp_driver_gpio esp_driver_i2c esp_driver_pcnt esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
