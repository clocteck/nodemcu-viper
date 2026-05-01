idf_component_register(
    SRCS
        "viper.cpp"
        "viper_module.c"
        "../../src/asmxtensa.c"
        "../../src/viper_cparser.cpp"
        "../../src/viper_native.cpp"
    INCLUDE_DIRS
        "."
        "../../src"
    PRIV_REQUIRES
        lua
        modules
        base_nodemcu
        heap
        esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

if(CONFIG_NODEMCU_CMODULE_VIPER)
    target_link_libraries(${COMPONENT_LIB} "-u VIPER_module_selected1")
endif()
