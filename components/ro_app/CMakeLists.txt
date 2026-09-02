idf_component_register(SRCS "src/app_v2.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ro_core ro_hardware ro_services ro_ui ro_network esp_timer esp_system esp_app_format app_update)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
