idf_component_register(
    SRCS
        "src/clock_plugin.cpp"
        "src/quote_provider.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        # The quote under the date needs HTTPS, the api-ninjas key and the cache. The clock
        # itself still needs none of them — see requiresNetwork() in clock_plugin.hpp.
        dashboard_network
        dashboard_storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
