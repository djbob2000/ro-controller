idf_component_register(SRCS "src/ui.cpp" INCLUDE_DIRS "include" REQUIRES ro_core ro_hardware)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
